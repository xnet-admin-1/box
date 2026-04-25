package main

import (
	"crypto/subtle"
	"embed"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"

	"github.com/creack/pty"
	"github.com/gorilla/websocket"
)

//go:embed index.html
var static embed.FS

//go:embed static
var staticDir embed.FS

//go:embed term.html
var termPage []byte

var upgrader = websocket.Upgrader{CheckOrigin: func(r *http.Request) bool { return true }}

const (
	msgData      = 0x01
	msgResize    = 0x04
	msgHeartbeat = 0x07
	ringSize     = 64 * 1024
)

type termSession struct {
	mu   sync.Mutex
	ptmx *os.File
	cmd  *exec.Cmd
	ring [ringSize]byte
	rpos int // next write position
	rlen int // total bytes written (capped at ringSize)
	conn *websocket.Conn
}

var sess termSession

func (s *termSession) init() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.ptmx != nil {
		return nil
	}
	cmd := exec.Command("/bin/bash", "-l")
	cmd.Env = append(os.Environ(), "TERM=xterm-256color", "LANG=en_US.UTF-8")
	ptmx, err := pty.StartWithSize(cmd, &pty.Winsize{Cols: 120, Rows: 30})
	if err != nil {
		return err
	}
	s.ptmx = ptmx
	s.cmd = cmd

	// PTY reader — runs forever, writes to ring + active conn
	go func() {
		buf := make([]byte, 32769)
		buf[0] = msgData
		for {
			n, err := ptmx.Read(buf[1:])
			if err != nil {
				s.mu.Lock()
				s.ptmx = nil
				s.cmd = nil
				s.rlen = 0
				s.rpos = 0
				s.mu.Unlock()
				return
			}
			s.mu.Lock()
			// Write to ring buffer
			for i := 0; i < n; i++ {
				s.ring[s.rpos] = buf[1+i]
				s.rpos = (s.rpos + 1) % ringSize
			}
			s.rlen += n
			if s.rlen > ringSize {
				s.rlen = ringSize
			}
			// Write to active connection
			c := s.conn
			s.mu.Unlock()
			if c != nil {
				c.WriteMessage(websocket.BinaryMessage, buf[:n+1])
			}
		}
	}()
	return nil
}

func (s *termSession) replay(conn *websocket.Conn) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.rlen == 0 {
		return
	}
	size := s.rlen
	if size > ringSize {
		size = ringSize
	}
	start := (s.rpos - size + ringSize) % ringSize
	buf := make([]byte, 1+size)
	buf[0] = msgData
	for i := 0; i < size; i++ {
		buf[1+i] = s.ring[(start+i)%ringSize]
	}
	conn.WriteMessage(websocket.BinaryMessage, buf)
}

func (s *termSession) attach(conn *websocket.Conn) {
	s.mu.Lock()
	s.conn = conn
	s.mu.Unlock()
}

func (s *termSession) detach(conn *websocket.Conn) {
	s.mu.Lock()
	if s.conn == conn {
		s.conn = nil
	}
	s.mu.Unlock()
}

func handleTerm(w http.ResponseWriter, r *http.Request) {
	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		return
	}
	defer conn.Close()

	conn.SetReadDeadline(time.Now().Add(90 * time.Second))
	conn.SetPongHandler(func(string) error {
		conn.SetReadDeadline(time.Now().Add(90 * time.Second))
		return nil
	})

	if err := sess.init(); err != nil {
		conn.WriteMessage(websocket.BinaryMessage, append([]byte{msgData}, []byte("pty error: "+err.Error())...))
		return
	}

	// Wait for initial resize from client before replay
	conn.SetReadDeadline(time.Now().Add(5 * time.Second))
	_, msg, err2 := conn.ReadMessage()
	if err2 == nil && len(msg) >= 5 && msg[0] == msgResize {
		cols := uint16(msg[1])<<8 | uint16(msg[2])
		rows := uint16(msg[3])<<8 | uint16(msg[4])
		if cols > 0 && cols < 500 && rows > 0 && rows < 200 {
			pty.Setsize(sess.ptmx, &pty.Winsize{Cols: cols, Rows: rows})
		}
	}
	conn.SetReadDeadline(time.Now().Add(90 * time.Second))

	sess.replay(conn)
	sess.attach(conn)
	defer sess.detach(conn)

	closed := make(chan struct{})

	// ping
	go func() {
		t := time.NewTicker(25 * time.Second)
		defer t.Stop()
		for {
			select {
			case <-closed:
				return
			case <-t.C:
				conn.WriteControl(websocket.PingMessage, nil, time.Now().Add(5*time.Second))
			}
		}
	}()

	// ws → pty
	for {
		_, msg, err := conn.ReadMessage()
		if err != nil {
			break
		}
		if len(msg) == 0 {
			continue
		}
		switch msg[0] {
		case msgData:
			if len(msg) > 1 && sess.ptmx != nil {
				sess.ptmx.Write(msg[1:])
			}
		case msgResize:
			if len(msg) >= 5 && sess.ptmx != nil {
				cols := uint16(msg[1])<<8 | uint16(msg[2])
				rows := uint16(msg[3])<<8 | uint16(msg[4])
				if cols > 0 && cols < 500 && rows > 0 && rows < 200 {
					pty.Setsize(sess.ptmx, &pty.Winsize{Cols: cols, Rows: rows})
				}
			}
		case msgHeartbeat:
			conn.SetReadDeadline(time.Now().Add(90 * time.Second))
			conn.WriteMessage(websocket.BinaryMessage, []byte{msgHeartbeat})
		}
	}
	close(closed)
}

const addr = ":9800"

var dataDir = "/home/xnet-admin/ai/drop"
var credFile = "/home/xnet-admin/ai/.creds"

func init() {
	if v := os.Getenv("DROP_DATA"); v != "" {
		dataDir = v
	}
	if v := os.Getenv("DROP_CREDS"); v != "" {
		credFile = v
	}
}

var creds struct {
	User string `json:"user"`
	Pass string `json:"pass"`
}

func loadCreds() {
	creds.User = "user-x"
	creds.Pass = "!1nfer1"
	data, err := os.ReadFile(credFile)
	if err == nil {
		json.Unmarshal(data, &creds)
	}
}

func saveCreds() {
	data, _ := json.Marshal(creds)
	os.WriteFile(credFile, data, 0600)
}

type FileInfo struct {
	Name    string `json:"name"`
	Size    int64  `json:"size"`
	ModTime int64  `json:"modTime"`
	IsImage bool   `json:"isImage"`
}

func auth(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		u, p, ok := r.BasicAuth()
		if !ok || subtle.ConstantTimeCompare([]byte(u), []byte(creds.User)) != 1 || subtle.ConstantTimeCompare([]byte(p), []byte(creds.Pass)) != 1 {
			w.Header().Set("WWW-Authenticate", `Basic realm="drop"`)
			http.Error(w, "Unauthorized", 401)
			return
		}
		next(w, r)
	}
}

func isImage(name string) bool {
	ext := strings.ToLower(filepath.Ext(name))
	switch ext {
	case ".jpg", ".jpeg", ".png", ".gif", ".webp", ".svg", ".bmp", ".heic":
		return true
	}
	return false
}

func listFiles(w http.ResponseWriter, r *http.Request) {
	entries, _ := os.ReadDir(dataDir)
	files := []FileInfo{}
	for _, e := range entries {
		if e.IsDir() {
			continue
		}
		info, err := e.Info()
		if err != nil {
			continue
		}
		files = append(files, FileInfo{
			Name:    e.Name(),
			Size:    info.Size(),
			ModTime: info.ModTime().UnixMilli(),
			IsImage: isImage(e.Name()),
		})
	}
	sort.Slice(files, func(i, j int) bool { return files[i].ModTime > files[j].ModTime })
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(files)
}

func upload(w http.ResponseWriter, r *http.Request) {
	r.ParseMultipartForm(100 << 20) // 100MB
	file, header, err := r.FormFile("file")
	if err != nil {
		http.Error(w, err.Error(), 400)
		return
	}
	defer file.Close()
	name := filepath.Base(header.Filename)
	dst, err := os.Create(filepath.Join(dataDir, name))
	if err != nil {
		http.Error(w, err.Error(), 500)
		return
	}
	defer dst.Close()
	io.Copy(dst, file)
	w.WriteHeader(200)
	fmt.Fprintf(w, `{"ok":true,"name":"%s"}`, name)
}

func download(w http.ResponseWriter, r *http.Request) {
	name := filepath.Base(r.URL.Path[len("/api/files/"):])
	path := filepath.Join(dataDir, name)
	if _, err := os.Stat(path); os.IsNotExist(err) {
		http.Error(w, "Not found", 404)
		return
	}
	http.ServeFile(w, r, path)
}

func deleteFile(w http.ResponseWriter, r *http.Request) {
	name := filepath.Base(r.URL.Path[len("/api/files/"):])
	path := filepath.Join(dataDir, name)
	if err := os.Remove(path); err != nil {
		http.Error(w, err.Error(), 500)
		return
	}
	w.WriteHeader(200)
	fmt.Fprint(w, `{"ok":true}`)
}

func main() {
	os.MkdirAll(dataDir, 0755)
	loadCreds()

	http.HandleFunc("/", auth(func(w http.ResponseWriter, r *http.Request) {
		data, _ := static.ReadFile("index.html")
		w.Header().Set("Content-Type", "text/html")
		w.Write(data)
	}))
	http.HandleFunc("/api/creds", auth(func(w http.ResponseWriter, r *http.Request) {
		if r.Method == "POST" {
			var body struct {
				User string `json:"user"`
				Pass string `json:"pass"`
			}
			if json.NewDecoder(r.Body).Decode(&body) != nil || body.User == "" || body.Pass == "" {
				http.Error(w, `{"error":"user and pass required"}`, 400)
				return
			}
			creds.User = body.User
			creds.Pass = body.Pass
			saveCreds()
			fmt.Fprint(w, `{"ok":true}`)
		} else {
			fmt.Fprintf(w, `{"user":"%s"}`, creds.User)
		}
	}))
	http.HandleFunc("/api/files", auth(func(w http.ResponseWriter, r *http.Request) {
		switch r.Method {
		case "GET":
			listFiles(w, r)
		case "POST":
			upload(w, r)
		default:
			http.Error(w, "Method not allowed", 405)
		}
	}))
	http.HandleFunc("/api/files/", auth(func(w http.ResponseWriter, r *http.Request) {
		switch r.Method {
		case "GET":
			download(w, r)
		case "DELETE":
			deleteFile(w, r)
		default:
			http.Error(w, "Method not allowed", 405)
		}
	}))

	http.Handle("/static/", http.FileServer(http.FS(staticDir)))
	http.HandleFunc("/term", auth(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		w.Write(termPage)
	}))
	http.HandleFunc("/ws/term", auth(handleTerm))

	fmt.Printf("Drop running on %s (%s)\n", addr, time.Now().Format("15:04:05"))
	http.ListenAndServe(addr, nil)
}
