package main

import (
    "bufio"
    "strconv"
    "encoding/json"
    "fmt"
    "os"
    "os/exec"
    "path/filepath"
    "time"

    "github.com/sipeed/picoclaw/pkg/cron"
)

type request struct {
    Method string
    Action string
    ID string
    Schedule string
}

func handle(cs *cron.CronService, r request) (any, error) {
    switch r.Method {
    case "list_tasks":
        return cs.ListJobs(true), nil
    case "cancel_task":
        if !cs.RemoveJob(r.ID) { return nil, fmt.Errorf("unknown task ID") }
        return "cancelled", nil
    case "create_task":
        if r.Action != "wake_pc" && r.Action != "time" { return nil, fmt.Errorf("unknown action") }
        var schedule cron.CronSchedule
        if t, err := time.Parse(time.RFC3339, r.Schedule); err == nil {
            if !t.After(time.Now()) { return nil, fmt.Errorf("time must be in the future") }
            ms := t.UnixMilli()
            schedule = cron.CronSchedule{Kind:"at", AtMS:&ms}
        } else {
            t, err := time.Parse("15:04", r.Schedule)
            if err != nil { return nil, fmt.Errorf("expected HH:MM or RFC3339") }
            schedule = cron.CronSchedule{Kind:"cron", Expr:fmt.Sprintf("%d %d * * *", t.Minute(), t.Hour()), TZ:"Asia/Shanghai"}
        }
        return cs.AddJob(r.Action, schedule, r.Action, "", "")
    }
    return nil, fmt.Errorf("unknown method")
}

func main() {
    dir, err := os.UserConfigDir()
    if err != nil { panic(err) }
    dir = filepath.Join(dir, "xiaozhi-picoclaw")
    if err := os.MkdirAll(dir, 0700); err != nil { panic(err) }
    cs := cron.NewCronService(filepath.Join(dir, "jobs.json"), func(j *cron.CronJob) (string, error) {
        if j.Payload.Message != "time" && j.Payload.Message != "wake_pc" { return "", fmt.Errorf("unknown action") }
        out, err := exec.Command(os.Args[1], os.Args[2], "--action", j.Payload.Message).Output()
        return string(out), err
    })
    if err := cs.Start(); err != nil { panic(err) }
    defer cs.Stop()
    // Dedicated descriptor keeps library log output out of the response stream.
    fd, err := strconv.Atoi(os.Args[3])
    if err != nil { panic(err) }
    replies := json.NewEncoder(os.NewFile(uintptr(fd), "replies"))
    scanner := bufio.NewScanner(os.Stdin)
    for scanner.Scan() {
        var r request
        err := json.Unmarshal(scanner.Bytes(), &r)
        var result any
        if err == nil { result, err = handle(cs, r) }
        reply := map[string]any{"result":result}
        if err != nil { reply["error"] = err.Error() }
        if err := replies.Encode(reply); err != nil { return }
    }
}

