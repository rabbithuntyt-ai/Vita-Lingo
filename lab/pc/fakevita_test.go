package main

// A fake Vita on 127.0.0.1 speaking the same protocols as the Vita Lab
// plugins: commands (1338), the screen (1339) and FTP (1337), with files kept
// in memory. Tests drive it through the real client code.

import (
	"archive/zip"
	"bufio"
	"bytes"
	"encoding/binary"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"testing"
	"time"
)

type fakeVita struct {
	mu    sync.Mutex
	cmds  []string
	files map[string][]byte // "/ux0:/data/x" -> contents
	dirs  map[string]bool
	ls    []net.Listener
}

func startFake(t *testing.T) *fakeVita {
	f := &fakeVita{files: map[string][]byte{}, dirs: map[string]bool{"/ux0:": true, "/ux0:/data": true, "/ux0:/app": true}}
	f.files["/ux0:/data/LNGO00001_crash.psp2dmp"] = []byte("dump")
	for _, p := range []int{cmdPort, screenPort, ftpPort} {
		l, err := net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", p))
		if err != nil {
			t.Fatalf("port %d: %v", p, err)
		}
		f.ls = append(f.ls, l)
		port := p
		go func() {
			for {
				c, err := l.Accept()
				if err != nil {
					return
				}
				switch port {
				case cmdPort:
					go f.cmd(c)
				case screenPort:
					go f.screen(c)
				case ftpPort:
					go f.ftp(c)
				}
			}
		}()
	}
	t.Cleanup(func() {
		for _, l := range f.ls {
			l.Close()
		}
	})
	return f
}

func (f *fakeVita) cmd(c net.Conn) {
	defer c.Close()
	line, _ := bufio.NewReader(c).ReadString('\n')
	line = strings.TrimSpace(line)
	f.mu.Lock()
	f.cmds = append(f.cmds, line)
	f.mu.Unlock()
	switch {
	case line == "stats":
		io.WriteString(c, "fps=30 flips=1234 cpu=91,40,12,3 arm=444 bus=222 gpu=222 xbar=166 app=PCSX00001 kernel=ok\n")
	case line == "version":
		io.WriteString(c, "vitacompanion 1.8\n")
	case strings.HasPrefix(line, "promote "):
		io.WriteString(c, "Promoted.\n")
	case strings.HasPrefix(line, "nope"):
		io.WriteString(c, "Error: Unknown command.\n")
	default:
		io.WriteString(c, "ok\n")
	}
}

func (f *fakeVita) screen(c net.Conn) {
	defer c.Close()
	line, _ := bufio.NewReader(c).ReadString('\n')
	step := 1
	fmt.Sscanf(line, "shot %d", &step)
	w, h := 960/step, 544/step
	head := []uint32{0, 1, uint32(w), uint32(h), 960, 544, 0x42, 0, 30, 999}
	b := new(bytes.Buffer)
	b.WriteString("VLAB")
	binary.Write(b, binary.LittleEndian, head[1:])
	pix := make([]byte, w*h*2)
	for i := 0; i < w*h; i++ {
		// pure red on the left half, pure blue on the right
		v := uint16(0xF800)
		if i%w >= w/2 {
			v = 0x001F
		}
		binary.LittleEndian.PutUint16(pix[2*i:], v)
	}
	b.Write(pix)
	c.Write(b.Bytes())
}

func (f *fakeVita) ftp(c net.Conn) {
	defer c.Close()
	r := bufio.NewReader(c)
	say := func(s string) { io.WriteString(c, s+"\r\n") }
	say("220 FTPVita")
	var pasv net.Listener
	for {
		line, err := r.ReadString('\n')
		if err != nil {
			return
		}
		line = strings.TrimRight(line, "\r\n")
		verb, arg, _ := strings.Cut(line, " ")
		data := func() net.Conn {
			if pasv == nil {
				return nil
			}
			d, err := pasv.Accept()
			pasv.Close()
			pasv = nil
			if err != nil {
				return nil
			}
			return d
		}
		f.mu.Lock()
		switch verb {
		case "USER":
			say("331 ok")
		case "PASS":
			say("230 ok")
		case "TYPE":
			say("200 ok")
		case "QUIT":
			f.mu.Unlock()
			say("221 bye")
			return
		case "PASV":
			pasv, _ = net.Listen("tcp", "127.0.0.1:0")
			p := pasv.Addr().(*net.TCPAddr).Port
			say(fmt.Sprintf("227 Entering Passive Mode (127,0,0,1,%d,%d)", p/256, p%256))
		case "MKD":
			if f.dirs[arg] {
				say("550 exists")
			} else if !f.dirs[path.Dir(arg)] {
				say("550 no parent " + path.Dir(arg))
			} else {
				f.dirs[arg] = true
				say("257 made")
			}
		case "STOR":
			if !f.dirs[path.Dir(arg)] {
				say("550 no folder")
				break
			}
			say("150 go")
			f.mu.Unlock()
			d := data()
			b, _ := io.ReadAll(d)
			d.Close()
			f.mu.Lock()
			f.files[arg] = b
			say("226 done")
		case "RETR":
			b, ok := f.files[arg]
			if !ok {
				say("550 no file")
				break
			}
			say("150 go")
			f.mu.Unlock()
			d := data()
			d.Write(b)
			d.Close()
			f.mu.Lock()
			say("226 done")
		case "LIST":
			var lines []string
			if b, ok := f.files[arg]; ok {
				lines = append(lines, fmt.Sprintf("-rw-r--r-- 1 vita vita %d Jan 1 00:00 %s", len(b), path.Base(arg)))
			} else if !f.dirs[arg] {
				say("550 no such folder")
				break
			}
			for p, b := range f.files {
				if path.Dir(p) == arg {
					lines = append(lines, fmt.Sprintf("-rw-r--r-- 1 vita vita %d Jan 1 00:00 %s", len(b), path.Base(p)))
				}
			}
			for p := range f.dirs {
				if path.Dir(p) == arg && p != arg {
					lines = append(lines, fmt.Sprintf("drwxr-xr-x 1 vita vita 0 Jan 1 00:00 %s", path.Base(p)))
				}
			}
			sort.Strings(lines)
			say("150 go")
			f.mu.Unlock()
			d := data()
			io.WriteString(d, strings.Join(lines, "\r\n")+"\r\n")
			d.Close()
			f.mu.Lock()
			say("226 done")
		case "DELE":
			delete(f.files, arg)
			say("250 gone")
		case "RMD":
			delete(f.dirs, arg)
			say("250 gone")
		default:
			say("502 not here")
		}
		f.mu.Unlock()
	}
}

func (f *fakeVita) last() string {
	f.mu.Lock()
	defer f.mu.Unlock()
	if len(f.cmds) == 0 {
		return ""
	}
	return f.cmds[len(f.cmds)-1]
}

func TestStatsAndCommands(t *testing.T) {
	f := startFake(t)
	v := &Vita{Host: "127.0.0.1"}
	s, err := v.Stats()
	if err != nil || s.FPS != 30 || s.CPU != [4]int{91, 40, 12, 3} || s.ARM != 444 || s.App != "PCSX00001" {
		t.Fatalf("stats: %+v %v", s, err)
	}
	if _, err := v.Cmd("nope"); err == nil {
		t.Fatal("an Error reply should be an error")
	}
	cases := []struct {
		do   func() error
		want string
	}{
		{func() error { return v.Press("cross", 100) }, "press cross; wait 100ms; release cross"},
		{func() error { return v.Press("l+r", 500) }, "press l; press r; wait 500ms; release l; release r"},
		{func() error { return v.Tap(480, 272, 80) }, "press front-touch 0 960 544; wait 80ms; release front-touch 0"},
		{func() error { return v.Tap(960, 544, 80) }, "press front-touch 0 1919 1087; wait 80ms; release front-touch 0"},
		{func() error { return v.Stick("right", 255, 128, 300) }, "press right-stick 255 128; wait 300ms; release right-stick"},
	}
	for _, c := range cases {
		if err := c.do(); err != nil {
			t.Fatal(err)
		}
		if got := f.last(); got != c.want {
			t.Errorf("sent %q, want %q", got, c.want)
		}
	}
	if err := v.Swipe(100, 100, 800, 100, 1000); err != nil {
		t.Fatal(err)
	}
	sw := f.last()
	if n := strings.Count(sw, ";") + 1; n > 32 {
		t.Errorf("swipe chains %d commands; the Vita runs at most 32", n)
	}
	if !strings.HasPrefix(sw, "press front-touch 0 200 200") || !strings.HasSuffix(sw, "release front-touch 0") {
		t.Errorf("swipe: %s", sw)
	}
	if err := v.Press("jump", 100); err == nil {
		t.Error("an unknown button should be refused")
	}
}

func TestShot(t *testing.T) {
	startFake(t)
	v := &Vita{Host: "127.0.0.1"}
	for _, step := range []int{1, 2} {
		fr, err := v.Shot(step)
		if err != nil {
			t.Fatal(err)
		}
		if fr.W != 960/step || fr.H != 544/step || fr.FPS != 30 {
			t.Fatalf("frame %dx%d fps %d", fr.W, fr.H, fr.FPS)
		}
		img := fr.Image()
		if c := img.RGBAAt(1, 1); c.R != 255 || c.G != 0 || c.B != 0 {
			t.Errorf("left pixel %v, want red", c)
		}
		if c := img.RGBAAt(fr.W-1, fr.H-1); c.B != 255 || c.R != 0 {
			t.Errorf("right pixel %v, want blue", c)
		}
	}
}

func sfo(title string) []byte {
	// one key, TITLE_ID
	keys := []byte("TITLE_ID\x00\x00\x00\x00")
	data := append([]byte(title), 0, 0, 0)
	b := new(bytes.Buffer)
	b.WriteString("\x00PSF")
	binary.Write(b, binary.LittleEndian, []uint32{0x101, 20 + 16, 20 + 16 + uint32(len(keys)), 1})
	binary.Write(b, binary.LittleEndian, []uint16{0, 0x0204})
	binary.Write(b, binary.LittleEndian, []uint32{uint32(len(title) + 1), uint32(len(data)), 0})
	b.Write(keys)
	b.Write(data)
	return b.Bytes()
}

func TestInstallUninstallPush(t *testing.T) {
	f := startFake(t)
	v := &Vita{Host: "127.0.0.1"}
	dir := t.TempDir()
	vpk := filepath.Join(dir, "game.vpk")
	out, _ := os.Create(vpk)
	zw := zip.NewWriter(out)
	files := map[string]string{"eboot.bin": "EBOOT", "sce_sys/param.sfo": string(sfo("TEST00001")),
		"sce_sys/livearea/contents/bg.png": "PNG", "data/big.bin": strings.Repeat("x", 300000)}
	for name, body := range files {
		w, _ := zw.Create(name)
		io.WriteString(w, body)
	}
	zw.Close()
	out.Close()
	title, err := v.Install(vpk, nil)
	if err != nil || title != "TEST00001" {
		t.Fatalf("install: %s %v", title, err)
	}
	for name, body := range files {
		got := f.files["/ux0:/data/vitalab/install/TEST00001/"+name]
		if string(got) != body {
			t.Errorf("%s: %d bytes on the Vita, want %d", name, len(got), len(body))
		}
	}
	if got := f.last(); got != "promote ux0:data/vitalab/install/TEST00001" {
		t.Errorf("then sent %q", got)
	}
	// installing again clears the stage first
	if _, err := v.Install(vpk, nil); err != nil {
		t.Fatal(err)
	}
	if err := v.Uninstall("TEST00001"); err != nil || f.last() != "delete TEST00001" {
		t.Errorf("uninstall sent %q (%v)", f.last(), err)
	}
	// swapping the program
	f.dirs["/ux0:/app/TEST00001"] = true
	eb := filepath.Join(dir, "eboot.bin")
	os.WriteFile(eb, []byte("NEW"), 0644)
	if err := v.PushEboot("TEST00001", eb, true); err != nil {
		t.Fatal(err)
	}
	if string(f.files["/ux0:/app/TEST00001/eboot.bin"]) != "NEW" || f.last() != "launch TEST00001" {
		t.Errorf("push-eboot: %q, last %q", f.files["/ux0:/app/TEST00001/eboot.bin"], f.last())
	}
	// crash dumps
	l, err := v.Crashes()
	if err != nil || len(l) != 1 || l[0].Name != "LNGO00001_crash.psp2dmp" || l[0].Size != 4 {
		t.Errorf("crashes: %+v %v", l, err)
	}
	// rm of a single file and of a tree
	ftp, _ := v.FTP()
	defer ftp.Close()
	if err := ftp.RemoveAll("ux0:data/LNGO00001_crash.psp2dmp"); err != nil {
		t.Error(err)
	}
	if err := ftp.RemoveAll("ux0:data/vitalab"); err != nil {
		t.Error(err)
	}
	for p := range f.files {
		if strings.HasPrefix(p, "/ux0:/data/vitalab") || strings.HasSuffix(p, ".psp2dmp") {
			t.Errorf("%s still there", p)
		}
	}
}

func TestBenchAndScript(t *testing.T) {
	f := startFake(t)
	v := &Vita{Host: "127.0.0.1"}
	r, err := v.Bench(1200*time.Millisecond, nil)
	if err != nil || len(r.Samples) < 2 || r.FPSAvg != 30 || r.FPS1Low != 30 || r.CPUAvg[0] != 91 {
		t.Fatalf("bench: %+v %v", r, err)
	}
	script := filepath.Join(t.TempDir(), "test.txt")
	os.WriteFile(script, []byte("# a test\npress cross\nsleep 10ms\ntap 10 20\ncmd \"press up; release up\"\n"), 0644)
	if err := runScript(v, script); err != nil {
		t.Fatal(err)
	}
	if f.last() != "press up; release up" {
		t.Errorf("script ended with %q", f.last())
	}
}

func TestPanelGuard(t *testing.T) {
	startFake(t)
	v := &Vita{Host: "127.0.0.1"}
	l, _ := net.Listen("tcp", "127.0.0.1:0")
	port := l.Addr().(*net.TCPAddr).Port
	l.Close()
	go serveUI(v, fmt.Sprintf("127.0.0.1:%d", port), false)
	time.Sleep(200 * time.Millisecond)
	base := fmt.Sprintf("http://127.0.0.1:%d", port)
	get := func(p string, hdr bool, host string) int {
		req, _ := http.NewRequest("GET", base+p, nil)
		if hdr {
			req.Header.Set("X-Vitalab", "1")
		}
		if host != "" {
			req.Host = host
		}
		res, err := http.DefaultClient.Do(req)
		if err != nil {
			t.Fatal(err)
		}
		res.Body.Close()
		return res.StatusCode
	}
	if c := get("/", false, ""); c != 200 {
		t.Errorf("page: %d", c)
	}
	if c := get("/api/stats", false, ""); c != 403 {
		t.Errorf("API without the header: %d, want 403", c)
	}
	if c := get("/api/stats", true, "evil.example:80"); c != 403 {
		t.Errorf("API from another host name: %d, want 403", c)
	}
	if c := get("/api/stats", true, ""); c != 200 {
		t.Errorf("API from the panel: %d", c)
	}
	if c := get("/api/shot?step=2", true, ""); c != 200 {
		t.Errorf("shot: %d", c)
	}
	_ = httptest.NewRecorder
}
