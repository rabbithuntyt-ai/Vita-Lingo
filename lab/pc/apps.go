package main

// Installing, replacing and removing apps; benchmarks; crash dumps.

import (
	"archive/zip"
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"os"
	"path"
	"sort"
	"strings"
	"time"
)

const stageDir = "ux0:data/vitalab/install"

// TitleID reads TITLE_ID from a param.sfo.
func TitleID(sfo []byte) (string, error) {
	if len(sfo) < 20 || string(sfo[:4]) != "\x00PSF" {
		return "", errors.New("not a param.sfo")
	}
	keyTable := binary.LittleEndian.Uint32(sfo[8:])
	dataTable := binary.LittleEndian.Uint32(sfo[12:])
	n := binary.LittleEndian.Uint32(sfo[16:])
	for i := uint32(0); i < n; i++ {
		e := 20 + i*16
		if int(e+16) > len(sfo) {
			break
		}
		ko := keyTable + uint32(binary.LittleEndian.Uint16(sfo[e:]))
		dl := binary.LittleEndian.Uint32(sfo[e+4:])
		do := dataTable + binary.LittleEndian.Uint32(sfo[e+12:])
		if int(ko) >= len(sfo) || int(do+dl) > len(sfo) {
			continue
		}
		k := sfo[ko:]
		if z := bytes.IndexByte(k, 0); z >= 0 {
			k = k[:z]
		}
		if string(k) == "TITLE_ID" {
			return strings.TrimRight(string(sfo[do:do+dl]), "\x00"), nil
		}
	}
	return "", errors.New("param.sfo has no TITLE_ID")
}

// Install puts a VPK on the Vita: unpacked into a staging folder over FTP,
// then installed by the system's package promoter (as VitaShell does).
func (v *Vita) Install(vpkPath string, progress func(string)) (string, error) {
	zr, err := zip.OpenReader(vpkPath)
	if err != nil {
		return "", fmt.Errorf("can't open %s as a VPK: %w", vpkPath, err)
	}
	defer zr.Close()
	var sfo []byte
	for _, f := range zr.File {
		if strings.EqualFold(f.Name, "sce_sys/param.sfo") {
			r, _ := f.Open()
			sfo, _ = io.ReadAll(r)
			r.Close()
		}
	}
	title, err := TitleID(sfo)
	if err != nil {
		return "", fmt.Errorf("%s: %w", vpkPath, err)
	}
	ftp, err := v.FTP()
	if err != nil {
		return title, err
	}
	defer ftp.Close()
	stage := stageDir + "/" + title
	ftp.RemoveAll(stage) // a half-finished earlier attempt
	if err := ftp.MkdirAll(stage); err != nil {
		return title, err
	}
	made := map[string]bool{}
	var total, done int64
	for _, f := range zr.File {
		total += int64(f.UncompressedSize64)
	}
	start := time.Now()
	for _, f := range zr.File {
		if f.FileInfo().IsDir() {
			continue
		}
		dir := path.Dir(f.Name)
		if dir != "." && !made[dir] {
			if err := ftp.MkdirAll(stage + "/" + dir); err != nil {
				return title, err
			}
			made[dir] = true
		}
		r, err := f.Open()
		if err != nil {
			return title, err
		}
		err = ftp.Put(stage+"/"+f.Name, r)
		r.Close()
		if err != nil {
			return title, err
		}
		done += int64(f.UncompressedSize64)
		if progress != nil {
			progress(fmt.Sprintf("uploading %s: %d%% (%s)", title, done*100/max64(total, 1), f.Name))
		}
	}
	if progress != nil {
		progress(fmt.Sprintf("uploaded %.1f MB in %s; installing", float64(total)/1e6, time.Since(start).Round(time.Second)))
	}
	out, err := v.Cmd("promote " + stage)
	if err != nil {
		return title, fmt.Errorf("install failed: %s", out)
	}
	return title, nil
}

func max64(a, b int64) int64 {
	if a > b {
		return a
	}
	return b
}

func (v *Vita) Uninstall(title string) error {
	_, err := v.Cmd("delete " + title)
	return err
}

// PushEboot swaps just the program of an installed app (much quicker than a
// whole VPK while testing): quit it, upload eboot.bin, start it again.
func (v *Vita) PushEboot(title, local string, launch bool) error {
	data, err := os.ReadFile(local)
	if err != nil {
		return err
	}
	v.Cmd("quit " + title)
	time.Sleep(500 * time.Millisecond)
	ftp, err := v.FTP()
	if err != nil {
		return err
	}
	defer ftp.Close()
	if err := ftp.Put("ux0:app/"+title+"/eboot.bin", bytes.NewReader(data)); err != nil {
		return err
	}
	if launch {
		_, err = v.Cmd("launch " + title)
	}
	return err
}

// Bench samples the frame rate and CPU load for a while.
type BenchResult struct {
	Samples  []Stats
	FPSAvg   float64
	FPSMin   int
	FPSMax   int
	FPS1Low  int // the lowest 1% of samples
	CPUAvg   [4]float64
	Duration time.Duration
}

func (v *Vita) Bench(d time.Duration, each func(*Stats)) (*BenchResult, error) {
	res := &BenchResult{FPSMin: 1 << 30}
	start := time.Now()
	for time.Since(start) < d {
		s, err := v.Stats()
		if err != nil {
			return nil, err
		}
		res.Samples = append(res.Samples, *s)
		if each != nil {
			each(s)
		}
		time.Sleep(500 * time.Millisecond)
	}
	res.Duration = time.Since(start)
	if len(res.Samples) == 0 {
		return res, nil
	}
	var fps []int
	for _, s := range res.Samples {
		res.FPSAvg += float64(s.FPS)
		fps = append(fps, s.FPS)
		if s.FPS < res.FPSMin {
			res.FPSMin = s.FPS
		}
		if s.FPS > res.FPSMax {
			res.FPSMax = s.FPS
		}
		for c := 0; c < 4; c++ {
			res.CPUAvg[c] += float64(s.CPU[c])
		}
	}
	n := float64(len(res.Samples))
	res.FPSAvg /= n
	for c := range res.CPUAvg {
		res.CPUAvg[c] /= n
	}
	sort.Ints(fps)
	res.FPS1Low = fps[len(fps)/100]
	return res, nil
}

// Crash dumps: the Vita writes ux0:data/*.psp2dmp when an app crashes.
func (v *Vita) Crashes() ([]Entry, error) {
	ftp, err := v.FTP()
	if err != nil {
		return nil, err
	}
	defer ftp.Close()
	all, err := ftp.List("ux0:data")
	if err != nil {
		return nil, err
	}
	var out []Entry
	for _, e := range all {
		if !e.Dir && strings.HasSuffix(strings.ToLower(e.Name), ".psp2dmp") {
			out = append(out, e)
		}
	}
	return out, nil
}
