package main

// The control panel: a page on this PC only (127.0.0.1) showing the Vita's
// screen live, with taps, buttons, stats, clocks and app management.

import (
	_ "embed"
	"encoding/json"
	"fmt"
	"image/png"
	"io"
	"net"
	"net/http"
	"os"
	"os/exec"
	"runtime"
	"strconv"
	"sync"
	"time"
)

//go:embed ui.html
var uiHTML []byte

func jsonOut(w http.ResponseWriter, v any) {
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(v)
}

func fail(w http.ResponseWriter, err error) {
	http.Error(w, err.Error(), http.StatusBadGateway)
}

func serveUI(v *Vita, addr string, open bool) error {
	var shotMu sync.Mutex // one screenshot at a time
	mux := http.NewServeMux()
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Write(uiHTML)
	})
	mux.HandleFunc("/api/shot", func(w http.ResponseWriter, r *http.Request) {
		step, _ := strconv.Atoi(r.URL.Query().Get("step"))
		if step < 1 {
			step = 2
		}
		shotMu.Lock()
		f, err := v.Shot(step)
		shotMu.Unlock()
		if err != nil {
			fail(w, err)
			return
		}
		w.Header().Set("Content-Type", "image/png")
		w.Header().Set("Cache-Control", "no-store")
		w.Header().Set("X-Vita", v.Host)
		w.Header().Set("X-Fps", strconv.Itoa(int(f.FPS)))
		enc := png.Encoder{CompressionLevel: png.BestSpeed}
		enc.Encode(w, f.Image())
	})
	mux.HandleFunc("/api/stats", func(w http.ResponseWriter, r *http.Request) {
		s, err := v.Stats()
		if err != nil {
			fail(w, err)
			return
		}
		jsonOut(w, s)
	})
	mux.HandleFunc("/api/cmd", func(w http.ResponseWriter, r *http.Request) {
		var req struct{ Line string }
		json.NewDecoder(r.Body).Decode(&req)
		out, err := v.Cmd(req.Line)
		if err != nil {
			fail(w, err)
			return
		}
		jsonOut(w, out)
	})
	mux.HandleFunc("/api/tap", func(w http.ResponseWriter, r *http.Request) {
		var req struct{ X, Y int }
		json.NewDecoder(r.Body).Decode(&req)
		if err := v.Tap(req.X, req.Y, 80); err != nil {
			fail(w, err)
			return
		}
		jsonOut(w, "ok")
	})
	mux.HandleFunc("/api/swipe", func(w http.ResponseWriter, r *http.Request) {
		var req struct{ X1, Y1, X2, Y2 int }
		json.NewDecoder(r.Body).Decode(&req)
		if err := v.Swipe(req.X1, req.Y1, req.X2, req.Y2, 250); err != nil {
			fail(w, err)
			return
		}
		jsonOut(w, "ok")
	})
	mux.HandleFunc("/api/input", func(w http.ResponseWriter, r *http.Request) {
		var req struct {
			Button string
			Down   bool
		}
		json.NewDecoder(r.Body).Decode(&req)
		name, err := buttonName(req.Button)
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		verb := "release"
		if req.Down {
			verb = "press"
		}
		if _, err := v.Cmd(verb + " " + name); err != nil {
			fail(w, err)
			return
		}
		jsonOut(w, "ok")
	})
	mux.HandleFunc("/api/bench", func(w http.ResponseWriter, r *http.Request) {
		var req struct{ Seconds int }
		json.NewDecoder(r.Body).Decode(&req)
		if req.Seconds <= 0 || req.Seconds > 600 {
			req.Seconds = 30
		}
		res, err := v.Bench(time.Duration(req.Seconds)*time.Second, nil)
		if err != nil {
			fail(w, err)
			return
		}
		jsonOut(w, res)
	})
	mux.HandleFunc("/api/crashes", func(w http.ResponseWriter, r *http.Request) {
		l, err := v.Crashes()
		if err != nil {
			fail(w, err)
			return
		}
		if l == nil {
			l = []Entry{}
		}
		jsonOut(w, l)
	})
	mux.HandleFunc("/api/install", func(w http.ResponseWriter, r *http.Request) {
		tmp, err := os.CreateTemp("", "vitalab-*.vpk")
		if err != nil {
			fail(w, err)
			return
		}
		defer os.Remove(tmp.Name())
		io.Copy(tmp, r.Body)
		tmp.Close()
		title, err := v.Install(tmp.Name(), func(s string) { fmt.Println(s) })
		if err != nil {
			fail(w, err)
			return
		}
		jsonOut(w, map[string]string{"title": title})
	})
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		return err
	}
	port := strconv.Itoa(ln.Addr().(*net.TCPAddr).Port)
	// Only this page may drive the Vita: other websites open in the browser
	// can't add the custom header without permission (which is never given),
	// and a Host check stops DNS rebinding.
	guard := func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			host := r.Host
			if host != "127.0.0.1:"+port && host != "localhost:"+port {
				http.Error(w, "forbidden", http.StatusForbidden)
				return
			}
			if r.URL.Path != "/" && r.Header.Get("X-Vitalab") != "1" {
				http.Error(w, "forbidden", http.StatusForbidden)
				return
			}
			next.ServeHTTP(w, r)
		})
	}
	url := "http://" + ln.Addr().String() + "/"
	fmt.Printf("Vita Lab control panel: %s (Vita at %s). Close this window to stop.\n", url, v.Host)
	if open {
		openBrowser(url)
	}
	return http.Serve(ln, guard(mux))
}

func openBrowser(url string) {
	switch runtime.GOOS {
	case "windows":
		exec.Command("rundll32", "url.dll,FileProtocolHandler", url).Start()
	case "darwin":
		exec.Command("open", url).Start()
	default:
		exec.Command("xdg-open", url).Start()
	}
}
