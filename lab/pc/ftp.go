package main

// A small FTP client for the Vita's FTP server (port 1337): passive mode,
// binary transfers, and Vita paths like "ux0:/data/x".

import (
	"bufio"
	"fmt"
	"io"
	"net"
	"path"
	"strconv"
	"strings"
	"time"
)

type FTP struct {
	c    net.Conn
	r    *bufio.Reader
	host string
}

func (v *Vita) FTP() (*FTP, error) {
	c, err := net.DialTimeout("tcp", net.JoinHostPort(v.Host, strconv.Itoa(ftpPort)), 5*time.Second)
	if err != nil {
		return nil, fmt.Errorf("can't reach the Vita's FTP server: %w", err)
	}
	f := &FTP{c: c, r: bufio.NewReader(c), host: v.Host}
	if _, _, err := f.reply(); err != nil {
		c.Close()
		return nil, err
	}
	for _, cmd := range []string{"USER vitalab", "PASS vitalab", "TYPE I"} {
		if code, msg, err := f.do(cmd); err != nil || code >= 400 {
			c.Close()
			if err == nil {
				err = fmt.Errorf("%s: %d %s", cmd, code, msg)
			}
			return nil, err
		}
	}
	return f, nil
}

func (f *FTP) Close() {
	f.do("QUIT")
	f.c.Close()
}

// reply reads one (possibly multi-line) reply.
func (f *FTP) reply() (int, string, error) {
	f.c.SetReadDeadline(time.Now().Add(30 * time.Second))
	line, err := f.r.ReadString('\n')
	if err != nil {
		return 0, "", err
	}
	line = strings.TrimRight(line, "\r\n")
	if len(line) < 3 {
		return 0, line, fmt.Errorf("odd FTP reply %q", line)
	}
	code, _ := strconv.Atoi(line[:3])
	if len(line) > 3 && line[3] == '-' {
		end := line[:3] + " "
		for {
			l, err := f.r.ReadString('\n')
			if err != nil {
				return code, line, err
			}
			if strings.HasPrefix(l, end) {
				break
			}
		}
	}
	msg := ""
	if len(line) > 4 {
		msg = line[4:]
	}
	return code, msg, nil
}

func (f *FTP) do(cmd string) (int, string, error) {
	f.c.SetWriteDeadline(time.Now().Add(30 * time.Second))
	if _, err := io.WriteString(f.c, cmd+"\r\n"); err != nil {
		return 0, "", err
	}
	return f.reply()
}

// ftpPath turns "ux0:data/x" or "ux0:/data/x" into the server's "/ux0:/data/x".
func ftpPath(p string) string {
	p = strings.ReplaceAll(p, "\\", "/")
	if i := strings.Index(p, ":"); i > 0 && !strings.HasPrefix(p, "/") {
		dev, rest := p[:i+1], strings.TrimPrefix(p[i+1:], "/")
		return "/" + dev + "/" + rest
	}
	return p
}

func (f *FTP) pasv() (net.Conn, error) {
	code, msg, err := f.do("PASV")
	if err != nil || code != 227 {
		return nil, fmt.Errorf("PASV failed: %d %s %v", code, msg, err)
	}
	a, b := strings.Index(msg, "("), strings.Index(msg, ")")
	if a < 0 || b < a {
		return nil, fmt.Errorf("odd PASV reply %q", msg)
	}
	var n [6]int
	for i, s := range strings.Split(msg[a+1:b], ",") {
		if i < 6 {
			n[i], _ = strconv.Atoi(strings.TrimSpace(s))
		}
	}
	// use the address we already reach the Vita on, not the one it reports
	return net.DialTimeout("tcp", net.JoinHostPort(f.host, strconv.Itoa(n[4]*256+n[5])), 10*time.Second)
}

func (f *FTP) Mkdir(p string) error {
	code, msg, err := f.do("MKD " + ftpPath(p))
	if err != nil {
		return err
	}
	if code >= 400 && code != 550 { // 550: already there
		return fmt.Errorf("mkdir %s: %d %s", p, code, msg)
	}
	return nil
}

// MkdirAll makes p and its parents.
func (f *FTP) MkdirAll(p string) error {
	p = ftpPath(p)
	parts := strings.Split(strings.Trim(p, "/"), "/")
	cur := ""
	for i, part := range parts {
		cur += "/" + part
		if i == 0 {
			continue // the device itself (ux0:)
		}
		if err := f.Mkdir(cur); err != nil {
			return err
		}
	}
	return nil
}

func (f *FTP) Put(remote string, r io.Reader) error {
	d, err := f.pasv()
	if err != nil {
		return err
	}
	code, msg, err := f.do("STOR " + ftpPath(remote))
	if err != nil || (code != 150 && code != 125) {
		d.Close()
		return fmt.Errorf("upload %s: %d %s %v", remote, code, msg, err)
	}
	_, cerr := io.Copy(d, r)
	d.Close()
	code, msg, err = f.reply()
	if cerr != nil {
		return cerr
	}
	if err != nil || code >= 400 {
		return fmt.Errorf("upload %s: %d %s %v", remote, code, msg, err)
	}
	return nil
}

func (f *FTP) Get(remote string, w io.Writer) error {
	d, err := f.pasv()
	if err != nil {
		return err
	}
	code, msg, err := f.do("RETR " + ftpPath(remote))
	if err != nil || (code != 150 && code != 125) {
		d.Close()
		return fmt.Errorf("download %s: %d %s %v", remote, code, msg, err)
	}
	_, cerr := io.Copy(w, d)
	d.Close()
	code, msg, err = f.reply()
	if cerr != nil {
		return cerr
	}
	if err != nil || code >= 400 {
		return fmt.Errorf("download %s: %d %s %v", remote, code, msg, err)
	}
	return nil
}

type Entry struct {
	Name string
	Dir  bool
	Size int64
	Line string
}

// List lists a directory (Unix "ls -l" style lines, as the Vita sends them).
func (f *FTP) List(dir string) ([]Entry, error) {
	d, err := f.pasv()
	if err != nil {
		return nil, err
	}
	code, msg, err := f.do("LIST " + ftpPath(dir))
	if err != nil || (code != 150 && code != 125) {
		d.Close()
		return nil, fmt.Errorf("list %s: %d %s %v", dir, code, msg, err)
	}
	b, _ := io.ReadAll(d)
	d.Close()
	f.reply()
	var out []Entry
	for _, l := range strings.Split(string(b), "\n") {
		l = strings.TrimRight(l, "\r")
		fs := strings.Fields(l)
		if len(fs) < 9 {
			continue
		}
		name := strings.Join(fs[8:], " ")
		if name == "." || name == ".." {
			continue
		}
		size, _ := strconv.ParseInt(fs[4], 10, 64)
		out = append(out, Entry{Name: name, Dir: strings.HasPrefix(fs[0], "d"), Size: size, Line: l})
	}
	return out, nil
}

func (f *FTP) Delete(p string) error {
	code, msg, err := f.do("DELE " + ftpPath(p))
	if err != nil || code >= 400 {
		return fmt.Errorf("delete %s: %d %s %v", p, code, msg, err)
	}
	return nil
}

// RemoveAll deletes a file or a whole directory tree.
func (f *FTP) RemoveAll(p string) error {
	entries, err := f.List(p)
	if err != nil || (len(entries) == 1 && !entries[0].Dir && entries[0].Name == path.Base(ftpPath(p))) {
		return f.Delete(p) // a file, not a directory
	}
	for _, e := range entries {
		child := path.Join(ftpPath(p), e.Name)
		if e.Dir {
			if err := f.RemoveAll(child); err != nil {
				return err
			}
		} else if err := f.Delete(child); err != nil {
			return err
		}
	}
	code, msg, err := f.do("RMD " + ftpPath(p))
	if err != nil || code >= 400 {
		return fmt.Errorf("rmdir %s: %d %s %v", p, code, msg, err)
	}
	return nil
}
