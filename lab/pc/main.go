// vitalab: drive a PS Vita running the Vita Lab plugins from a PC.
//
// Run with no arguments for the control panel in your browser, or give a
// command (vitalab help) to script tests: screenshots, taps and buttons,
// installing and removing apps, frame-rate benchmarks, clocks, files and
// crash dumps.
package main

import (
	"bufio"
	"bytes"
	"errors"
	"flag"
	"fmt"
	"image/png"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

const version = "1.0"

const usage = `vitalab %s: control a PS Vita running Vita Lab, from this PC.

  vitalab                         open the control panel (live screen, taps, buttons, stats)
  vitalab find                    list Vitas running Vita Lab on this network

 seeing
  vitalab shot [file.png] [--step 2]   screenshot (step 2 = half size, quicker)
  vitalab stats                   frame rate, CPU load per core, clocks, app on screen
  vitalab bench <seconds> [--csv f]    sample frame rate and CPU load; prints a summary

 input (screen pixels are 960x544)
  vitalab press <button>[+<button>] [ms]   e.g. press cross, press l+r 500
  vitalab tap <x> <y> [ms]
  vitalab swipe <x1> <y1> <x2> <y2> [ms]
  vitalab stick <left|right> <x> <y> [ms]  0..255, 128 = centre
  buttons: cross circle square triangle up down left right l r start select ps

 apps
  vitalab install <file.vpk>      install (or update) a VPK
  vitalab uninstall <TITLEID>
  vitalab launch <TITLEID>  |  vitalab quit <TITLEID|all>  |  vitalab apps
  vitalab push-eboot <TITLEID> <eboot.bin> [--no-launch]   swap just the program and restart it

 system
  vitalab clock <cpu> <bus> <gpu> <xbar> | clock default     MHz, e.g. clock 444 222 222 166
  vitalab reboot
  vitalab crashes [--get <folder>]     list (or download) crash dumps (.psp2dmp)
  vitalab ls <vita path>  |  get <vita path> <file>  |  put <file> <vita path>  |  rm <vita path>
  vitalab cmd "<raw command>"     any vitacompanion command, e.g. "press cross; wait 100ms; release cross"
  vitalab script <file>           run commands from a file, one per line ("sleep 500ms" waits)

 options (before the command)
  --host <ip>    the Vita's address (otherwise found automatically and remembered)
  --port <n>     control panel port on this PC (default 7070)
`

func main() {
	fs := flag.NewFlagSet("vitalab", flag.ContinueOnError)
	host := fs.String("host", os.Getenv("VITALAB_HOST"), "")
	port := fs.Int("port", 7070, "")
	noOpen := fs.Bool("no-browser", false, "")
	fs.Usage = func() { fmt.Printf(usage, version) }
	if err := fs.Parse(os.Args[1:]); err != nil {
		os.Exit(2)
	}
	args := fs.Args()
	if len(args) > 0 && (args[0] == "help" || args[0] == "-h" || args[0] == "--help") {
		fmt.Printf(usage, version)
		return
	}
	if len(args) > 0 && args[0] == "find" {
		found, err := Discover(2 * time.Second)
		if err != nil {
			die(err)
		}
		if len(found) == 0 {
			fmt.Println("No Vita found. Check: same Wi-Fi, Vita Lab installed and the Vita rebooted, Vita awake.")
			os.Exit(1)
		}
		for _, f := range found {
			fmt.Println(f)
		}
		return
	}
	v := &Vita{Host: resolveHost(*host)}
	if len(args) == 0 {
		die(serveUI(v, fmt.Sprintf("127.0.0.1:%d", *port), !*noOpen))
	}
	if err := run(v, args); err != nil {
		die(err)
	}
}

func die(err error) {
	if err != nil {
		fmt.Fprintln(os.Stderr, "error:", err)
		os.Exit(1)
	}
}

func configFile() string {
	dir, err := os.UserConfigDir()
	if err != nil {
		return ""
	}
	return filepath.Join(dir, "vitalab", "host.txt")
}

// resolveHost: the --host flag, else the last Vita used, else whatever answers on the network.
func resolveHost(h string) string {
	cf := configFile()
	if h == "" && cf != "" {
		if b, err := os.ReadFile(cf); err == nil {
			h = strings.TrimSpace(string(b))
			// still there?
			if _, err := (&Vita{Host: h}).Cmd("version"); err != nil {
				h = ""
			}
		}
	}
	if h == "" {
		found, _ := Discover(2 * time.Second)
		if len(found) == 0 {
			die(errors.New("no Vita found on this network; pass --host <the IP shown in the Vita Lab app>"))
		}
		h = found[0]
	}
	if cf != "" {
		os.MkdirAll(filepath.Dir(cf), 0755)
		os.WriteFile(cf, []byte(h+"\n"), 0644)
	}
	return h
}

func atoi(s string) int {
	n, err := strconv.Atoi(s)
	if err != nil {
		die(fmt.Errorf("%q isn't a number", s))
	}
	return n
}

func optInt(args []string, i, def int) int {
	if i < len(args) {
		return atoi(args[i])
	}
	return def
}

// flagValue pulls "--name value" out of args.
func flagValue(args []string, name string) ([]string, string) {
	for i := 0; i < len(args)-1; i++ {
		if args[i] == name {
			v := args[i+1]
			return append(append([]string{}, args[:i]...), args[i+2:]...), v
		}
	}
	return args, ""
}

func hasFlag(args []string, name string) ([]string, bool) {
	for i, a := range args {
		if a == name {
			return append(append([]string{}, args[:i]...), args[i+1:]...), true
		}
	}
	return args, false
}

func need(args []string, n int, form string) {
	if len(args) < n {
		die(fmt.Errorf("usage: vitalab %s", form))
	}
}

func run(v *Vita, args []string) error {
	cmd, rest := args[0], args[1:]
	switch cmd {
	case "shot":
		rest, stepS := flagValue(rest, "--step")
		step := 1
		if stepS != "" {
			step = atoi(stepS)
		}
		name := fmt.Sprintf("vita-%s.png", time.Now().Format("20060102-150405.000"))
		if len(rest) > 0 {
			name = rest[0]
		}
		f, err := v.Shot(step)
		if err != nil {
			return err
		}
		out, err := os.Create(name)
		if err != nil {
			return err
		}
		defer out.Close()
		if err := png.Encode(out, f.Image()); err != nil {
			return err
		}
		fmt.Printf("%s (%dx%d of %dx%d, game at %d fps)\n", name, f.W, f.H, f.SrcW, f.SrcH, f.FPS)
	case "stats":
		s, err := v.Stats()
		if err != nil {
			return err
		}
		fmt.Printf("%d fps  CPU %d%% %d%% %d%% %d%%  clocks CPU %d bus %d GPU %d xbar %d  on screen: %s\n", s.FPS, s.CPU[0], s.CPU[1],
			s.CPU[2], s.CPU[3], s.ARM, s.Bus, s.GPU, s.Xbar, s.App)
	case "bench":
		rest, csv := flagValue(rest, "--csv")
		secs := optInt(rest, 0, 30)
		var lines bytes.Buffer
		lines.WriteString("seconds,fps,cpu0,cpu1,cpu2,cpu3,arm,gpu,app\n")
		start := time.Now()
		r, err := v.Bench(time.Duration(secs)*time.Second, func(s *Stats) {
			fmt.Printf("\r%3.0fs  %3d fps  CPU %3d%% %3d%% %3d%% %3d%%", time.Since(start).Seconds(), s.FPS, s.CPU[0], s.CPU[1], s.CPU[2], s.CPU[3])
			fmt.Fprintf(&lines, "%.1f,%d,%d,%d,%d,%d,%d,%d,%s\n", time.Since(start).Seconds(), s.FPS, s.CPU[0], s.CPU[1], s.CPU[2], s.CPU[3], s.ARM,
				s.GPU, s.App)
		})
		fmt.Println()
		if err != nil {
			return err
		}
		fmt.Printf("%d samples over %s: average %.1f fps, min %d, 1%% low %d, max %d; CPU average %.0f%% %.0f%% %.0f%% %.0f%%\n", len(r.Samples),
			r.Duration.Round(time.Second), r.FPSAvg, r.FPSMin, r.FPS1Low, r.FPSMax, r.CPUAvg[0], r.CPUAvg[1], r.CPUAvg[2], r.CPUAvg[3])
		if csv != "" {
			return os.WriteFile(csv, lines.Bytes(), 0644)
		}
	case "press":
		need(rest, 1, "press <button>[+<button>] [ms]")
		return v.Press(rest[0], optInt(rest, 1, 100))
	case "tap":
		need(rest, 2, "tap <x> <y> [ms]")
		return v.Tap(atoi(rest[0]), atoi(rest[1]), optInt(rest, 2, 80))
	case "swipe":
		need(rest, 4, "swipe <x1> <y1> <x2> <y2> [ms]")
		return v.Swipe(atoi(rest[0]), atoi(rest[1]), atoi(rest[2]), atoi(rest[3]), optInt(rest, 4, 300))
	case "stick":
		need(rest, 3, "stick <left|right> <x> <y> [ms]")
		return v.Stick(rest[0], atoi(rest[1]), atoi(rest[2]), optInt(rest, 3, 300))
	case "install":
		need(rest, 1, "install <file.vpk>")
		title, err := v.Install(rest[0], func(s string) { fmt.Println(s) })
		if err != nil {
			return err
		}
		fmt.Println("installed", title)
	case "uninstall":
		need(rest, 1, "uninstall <TITLEID>")
		if err := v.Uninstall(rest[0]); err != nil {
			return err
		}
		fmt.Println("uninstalled", rest[0])
	case "launch", "quit":
		need(rest, 1, cmd+" <TITLEID>")
		out, err := v.Cmd(cmd + " " + rest[0])
		fmt.Println(out)
		return err
	case "apps", "reboot", "version":
		out, err := v.Cmd(cmd)
		fmt.Println(out)
		return err
	case "push-eboot":
		rest, noLaunch := hasFlag(rest, "--no-launch")
		need(rest, 2, "push-eboot <TITLEID> <eboot.bin>")
		if err := v.PushEboot(rest[0], rest[1], !noLaunch); err != nil {
			return err
		}
		fmt.Println("replaced", rest[0], "eboot.bin")
	case "clock":
		need(rest, 1, "clock <cpu> <bus> <gpu> <xbar> | clock default")
		out, err := v.Cmd("clock " + strings.Join(rest, " "))
		fmt.Println(out)
		return err
	case "crashes":
		_, dir := flagValue(rest, "--get")
		l, err := v.Crashes()
		if err != nil {
			return err
		}
		if len(l) == 0 {
			fmt.Println("no crash dumps in ux0:data")
		}
		for _, e := range l {
			fmt.Printf("%s  %d bytes\n", e.Name, e.Size)
		}
		if dir != "" && len(l) > 0 {
			ftp, err := v.FTP()
			if err != nil {
				return err
			}
			defer ftp.Close()
			os.MkdirAll(dir, 0755)
			for _, e := range l {
				out, err := os.Create(filepath.Join(dir, e.Name))
				if err != nil {
					return err
				}
				err = ftp.Get("ux0:data/"+e.Name, out)
				out.Close()
				if err != nil {
					return err
				}
				fmt.Println("saved", filepath.Join(dir, e.Name))
			}
		}
	case "ls":
		need(rest, 1, "ls <vita path>")
		ftp, err := v.FTP()
		if err != nil {
			return err
		}
		defer ftp.Close()
		l, err := ftp.List(rest[0])
		if err != nil {
			return err
		}
		for _, e := range l {
			kind := " "
			if e.Dir {
				kind = "d"
			}
			fmt.Printf("%s %10d  %s\n", kind, e.Size, e.Name)
		}
	case "get":
		need(rest, 2, "get <vita path> <file>")
		ftp, err := v.FTP()
		if err != nil {
			return err
		}
		defer ftp.Close()
		out, err := os.Create(rest[1])
		if err != nil {
			return err
		}
		defer out.Close()
		return ftp.Get(rest[0], out)
	case "put":
		need(rest, 2, "put <file> <vita path>")
		in, err := os.Open(rest[0])
		if err != nil {
			return err
		}
		defer in.Close()
		ftp, err := v.FTP()
		if err != nil {
			return err
		}
		defer ftp.Close()
		return ftp.Put(rest[1], in)
	case "rm":
		need(rest, 1, "rm <vita path>")
		ftp, err := v.FTP()
		if err != nil {
			return err
		}
		defer ftp.Close()
		return ftp.RemoveAll(rest[0])
	case "cmd":
		need(rest, 1, `cmd "<command>"`)
		out, err := v.Cmd(strings.Join(rest, " "))
		fmt.Println(out)
		return err
	case "script":
		need(rest, 1, "script <file>")
		return runScript(v, rest[0])
	default:
		return fmt.Errorf("unknown command %q (vitalab help lists them)", cmd)
	}
	return nil
}

// runScript runs one vitalab command per line; "sleep 500ms" waits and
// lines starting with # are comments.
func runScript(v *Vita, file string) error {
	f, err := os.Open(file)
	if err != nil {
		return err
	}
	defer f.Close()
	sc := bufio.NewScanner(f)
	n := 0
	for sc.Scan() {
		n++
		line := strings.TrimSpace(sc.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		fields := splitArgs(line)
		if fields[0] == "sleep" && len(fields) > 1 {
			d, err := time.ParseDuration(fields[1])
			if err != nil {
				return fmt.Errorf("%s:%d: %v", file, n, err)
			}
			time.Sleep(d)
			continue
		}
		fmt.Printf("> %s\n", line)
		if err := run(v, fields); err != nil {
			return fmt.Errorf("%s:%d: %v", file, n, err)
		}
	}
	return sc.Err()
}

// splitArgs splits on spaces, keeping "quoted strings" together.
func splitArgs(s string) []string {
	var out []string
	var cur strings.Builder
	quoted := false
	for _, r := range s {
		switch {
		case r == '"':
			quoted = !quoted
		case r == ' ' && !quoted:
			if cur.Len() > 0 {
				out = append(out, cur.String())
				cur.Reset()
			}
		default:
			cur.WriteRune(r)
		}
	}
	if cur.Len() > 0 {
		out = append(out, cur.String())
	}
	return out
}
