package main

// Talking to the Vita: the command port (1338), the screen port (1339) and
// discovery (UDP 1340). See lab/vita/src/lab.c for the Vita side.

import (
	"bufio"
	"encoding/binary"
	"errors"
	"fmt"
	"image"
	"image/color"
	"io"
	"net"
	"strconv"
	"strings"
	"time"
)

const (
	ftpPort       = 1337
	cmdPort       = 1338
	screenPort    = 1339
	discoveryPort = 1340
)

type Vita struct {
	Host string
}

// Cmd sends one command line (several may be chained with ";") and returns
// the reply. The Vita answers one connection at a time, then closes it.
func (v *Vita) Cmd(line string) (string, error) {
	c, err := net.DialTimeout("tcp", net.JoinHostPort(v.Host, strconv.Itoa(cmdPort)), 5*time.Second)
	if err != nil {
		return "", fmt.Errorf("can't reach the Vita at %s: %w", v.Host, err)
	}
	defer c.Close()
	// long chains with waits take as long as they say
	c.SetDeadline(time.Now().Add(60 * time.Second))
	if _, err := io.WriteString(c, strings.TrimRight(line, "\r\n")+"\n"); err != nil {
		return "", err
	}
	b, err := io.ReadAll(c)
	if err != nil && len(b) == 0 {
		return "", err
	}
	out := strings.TrimRight(string(b), "\r\n")
	if strings.HasPrefix(out, "Error") {
		return out, errors.New(out)
	}
	return out, nil
}

// Frame is one screenshot: RGB565 pixels, scaled down by Step.
type Frame struct {
	W, H, SrcW, SrcH int
	PID, Format     uint32
	FPS, Flips      uint32
	Pix             []byte
	At              time.Time
}

func (f *Frame) Image() *image.RGBA {
	img := image.NewRGBA(image.Rect(0, 0, f.W, f.H))
	for i := 0; i < f.W*f.H; i++ {
		p := uint16(f.Pix[2*i]) | uint16(f.Pix[2*i+1])<<8
		r, g, b := uint8(p>>11)<<3, uint8(p>>5&63)<<2, uint8(p&31)<<3
		img.SetRGBA(i%f.W, i/f.W, color.RGBA{r | r>>5, g | g>>6, b | b>>5, 255})
	}
	return img
}

// Shot takes a screenshot of whatever the Vita shows. step 1 is full size
// (960x544), 2 is half size (quicker over Wi-Fi).
func (v *Vita) Shot(step int) (*Frame, error) {
	c, err := net.DialTimeout("tcp", net.JoinHostPort(v.Host, strconv.Itoa(screenPort)), 5*time.Second)
	if err != nil {
		return nil, fmt.Errorf("can't reach the Vita's screen port: %w", err)
	}
	defer c.Close()
	c.SetDeadline(time.Now().Add(20 * time.Second))
	fmt.Fprintf(c, "shot %d\n", step)
	r := bufio.NewReaderSize(c, 1<<16)
	var head [10]uint32
	if err := binary.Read(r, binary.LittleEndian, &head); err != nil {
		return nil, fmt.Errorf("no picture from the Vita: %w", err)
	}
	if head[0] != binary.LittleEndian.Uint32([]byte("VLAB")) {
		return nil, errors.New("the Vita answered with something other than a picture")
	}
	if code := int32(head[1]); code < 0 {
		return nil, fmt.Errorf("the Vita couldn't capture the screen: %s", shotError(code))
	}
	f := &Frame{W: int(head[2]), H: int(head[3]), SrcW: int(head[4]), SrcH: int(head[5]), PID: head[6], Format: head[7],
		FPS: head[8], Flips: head[9], At: time.Now()}
	if f.W <= 0 || f.H <= 0 || f.W > 1920 || f.H > 1088 {
		return nil, fmt.Errorf("odd picture size %dx%d", f.W, f.H)
	}
	f.Pix = make([]byte, f.W*f.H*2)
	if _, err := io.ReadFull(r, f.Pix); err != nil {
		return nil, fmt.Errorf("picture cut short: %w", err)
	}
	return f, nil
}

func shotError(code int32) string {
	switch code {
	case -1:
		return "error -1: the plugin on the Vita is an older Vita Lab; install the new VitaLab.vpk, choose Reinstall plugins, and reboot"
	case -102:
		return "error -102: the kernel plugin's screen reader didn't start; reboot the Vita"
	case -103:
		return "error -103: the display reports no framebuffer (is the screen off or asleep?)"
	case -105:
		return "error -105: couldn't read the framebuffer of the app on screen"
	case -106:
		return "error -106: couldn't hand the picture to the plugin"
	}
	return fmt.Sprintf("display driver error 0x%08X", uint32(code))
}

// Discover asks the local network for Vitas running Vita Lab.
func Discover(wait time.Duration) ([]string, error) {
	c, err := net.ListenUDP("udp4", &net.UDPAddr{})
	if err != nil {
		return nil, err
	}
	defer c.Close()
	targets := []*net.UDPAddr{{IP: net.IPv4bcast, Port: discoveryPort}}
	// also each interface's own broadcast address (some routers drop 255.255.255.255)
	if ifs, err := net.Interfaces(); err == nil {
		for _, i := range ifs {
			addrs, _ := i.Addrs()
			for _, a := range addrs {
				if n, ok := a.(*net.IPNet); ok && n.IP.To4() != nil && !n.IP.IsLoopback() {
					ip := n.IP.To4()
					b := make(net.IP, 4)
					for k := range b {
						b[k] = ip[k] | ^n.Mask[k]
					}
					targets = append(targets, &net.UDPAddr{IP: b, Port: discoveryPort})
				}
			}
		}
	}
	for _, t := range targets {
		c.WriteToUDP([]byte("VITALAB?"), t)
	}
	c.SetReadDeadline(time.Now().Add(wait))
	seen := map[string]bool{}
	var found []string
	buf := make([]byte, 128)
	for {
		n, from, err := c.ReadFromUDP(buf)
		if err != nil {
			break
		}
		if strings.HasPrefix(string(buf[:n]), "VITALAB ") && !seen[from.IP.String()] {
			seen[from.IP.String()] = true
			found = append(found, from.IP.String())
		}
	}
	return found, nil
}

// Stats is one reading of the Vita's performance.
type Stats struct {
	FPS, Flips          int
	CPU                 [4]int
	ARM, Bus, GPU, Xbar int
	App                 string
	Raw                 string
}

func (v *Vita) Stats() (*Stats, error) {
	out, err := v.Cmd("stats")
	if err != nil {
		return nil, err
	}
	s := &Stats{Raw: out}
	for _, kv := range strings.Fields(out) {
		k, val, ok := strings.Cut(kv, "=")
		if !ok {
			continue
		}
		n, _ := strconv.Atoi(val)
		switch k {
		case "fps":
			s.FPS = n
		case "flips":
			s.Flips = n
		case "cpu":
			for i, p := range strings.Split(val, ",") {
				if i < 4 {
					s.CPU[i], _ = strconv.Atoi(p)
				}
			}
		case "arm":
			s.ARM = n
		case "bus":
			s.Bus = n
		case "gpu":
			s.GPU = n
		case "xbar":
			s.Xbar = n
		case "app":
			s.App = val
		}
	}
	return s, nil
}

// ---- input ----

// Buttons the Vita understands (vitacompanion's names).
var buttonNames = map[string]string{
	"x": "cross", "cross": "cross", "o": "circle", "circle": "circle", "square": "square", "triangle": "triangle",
	"up": "up", "down": "down", "left": "left", "right": "right", "start": "start", "select": "select",
	"l": "l", "r": "r", "l1": "l", "r1": "r", "ps": "ps",
}

func buttonName(b string) (string, error) {
	n, ok := buttonNames[strings.ToLower(b)]
	if !ok {
		return "", fmt.Errorf("unknown button %q (try cross, circle, square, triangle, up, down, left, right, l, r, start, select, ps)", b)
	}
	return n, nil
}

// Press presses buttons together (e.g. "l+r") for ms milliseconds.
func (v *Vita) Press(combo string, ms int) error {
	var press, release []string
	for _, b := range strings.Split(combo, "+") {
		n, err := buttonName(b)
		if err != nil {
			return err
		}
		press = append(press, "press "+n)
		release = append(release, "release "+n)
	}
	_, err := v.Cmd(fmt.Sprintf("%s; wait %dms; %s", strings.Join(press, "; "), ms, strings.Join(release, "; ")))
	return err
}

// touch coordinates are twice the screen's (1920x1088 for 960x544)
func touchXY(x, y int) (int, int) {
	tx, ty := x*2, y*2
	if tx > 1919 {
		tx = 1919
	}
	if ty > 1087 {
		ty = 1087
	}
	if tx < 0 {
		tx = 0
	}
	if ty < 0 {
		ty = 0
	}
	return tx, ty
}

// Tap touches the front screen at (x, y) in screen pixels (960x544).
func (v *Vita) Tap(x, y, ms int) error {
	tx, ty := touchXY(x, y)
	_, err := v.Cmd(fmt.Sprintf("press front-touch 0 %d %d; wait %dms; release front-touch 0", tx, ty, ms))
	return err
}

// Swipe drags from (x1,y1) to (x2,y2) over ms milliseconds.
func (v *Vita) Swipe(x1, y1, x2, y2, ms int) error {
	steps := ms / 16
	if steps < 4 {
		steps = 4
	}
	if steps > 14 {
		steps = 14 // the Vita runs at most 32 chained commands at once
	}
	var parts []string
	for i := 0; i <= steps; i++ {
		x := x1 + (x2-x1)*i/steps
		y := y1 + (y2-y1)*i/steps
		tx, ty := touchXY(x, y)
		parts = append(parts, fmt.Sprintf("press front-touch 0 %d %d", tx, ty))
		if i < steps {
			parts = append(parts, fmt.Sprintf("wait %dms", ms/steps))
		}
	}
	parts = append(parts, "release front-touch 0")
	_, err := v.Cmd(strings.Join(parts, "; "))
	return err
}

// Stick holds an analog stick at (x, y), 0..255 with 128 the centre.
func (v *Vita) Stick(which string, x, y, ms int) error {
	target := "left-stick"
	if strings.HasPrefix(strings.ToLower(which), "r") {
		target = "right-stick"
	}
	_, err := v.Cmd(fmt.Sprintf("press %s %d %d; wait %dms; release %s", target, x, y, ms, target))
	return err
}
