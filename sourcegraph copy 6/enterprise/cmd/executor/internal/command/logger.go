package command

import (
	"bytes"
	"context"
	"fmt"
	"io"
	"strings"
	"sync"
	"time"

	"github.com/inconshreveable/log15"

	"github.com/sourcegraph/sourcegraph/enterprise/internal/executor/types"
	internalexecutor "github.com/sourcegraph/sourcegraph/internal/executor"
	"github.com/sourcegraph/sourcegraph/lib/errors"
)

// Logger tracks command invocations and stores the command's output and
// error stream values.
type Logger interface {
	// Flush waits until all entries have been written to the store and all
	// background goroutines that watch a log entry and possibly update it have
	// exited.
	Flush() error
	// Log redacts secrets from the given log entry and stores it.
	Log(key string, command []string) LogEntry
}

// LogEntry is returned by Logger.Log and implements the io.WriteCloser
// interface to allow clients to update the Out field of the ExecutionLogEntry.
//
// The Close() method *must* be called once the client is done writing log
// output to flush the entry to the database.
type LogEntry interface {
	io.WriteCloser
	Finalize(exitCode int)
	CurrentLogEntry() internalexecutor.ExecutionLogEntry
}

type entryHandle struct {
	logEntry internalexecutor.ExecutionLogEntry
	replacer *strings.Replacer

	done chan struct{}

	mu         sync.Mutex
	buf        *bytes.Buffer
	exitCode   *int
	durationMs *int
}

func (h *entryHandle) Write(p []byte) (n int, err error) {
	h.mu.Lock()
	defer h.mu.Unlock()
	return h.buf.Write(p)
}

func (h *entryHandle) Finalize(exitCode int) {
	h.mu.Lock()
	defer h.mu.Unlock()

	durationMs := int(time.Since(h.logEntry.StartTime) / time.Millisecond)
	h.exitCode = &exitCode
	h.durationMs = &durationMs
}

func (h *entryHandle) Close() error {
	close(h.done)
	return nil
}

func (h *entryHandle) CurrentLogEntry() internalexecutor.ExecutionLogEntry {
	logEntry := h.currentLogEntry()
	redact(&logEntry, h.replacer)
	return logEntry
}

func (h *entryHandle) currentLogEntry() internalexecutor.ExecutionLogEntry {
	h.mu.Lock()
	defer h.mu.Unlock()

	logEntry := h.logEntry
	logEntry.ExitCode = h.exitCode
	logEntry.Out = h.buf.String()
	logEntry.DurationMs = h.durationMs
	return logEntry
}

type logger struct {
	store   ExecutionLogEntryStore
	done    chan struct{}
	handles chan *entryHandle

	job types.Job

	replacer *strings.Replacer

	errs   error
	errsMu sync.Mutex
}

// ExecutionLogEntryStore handle interactions with executor.Job logs.
type ExecutionLogEntryStore interface {
	AddExecutionLogEntry(ctx context.Context, job types.Job, entry internalexecutor.ExecutionLogEntry) (int, error)
	UpdateExecutionLogEntry(ctx context.Context, job types.Job, entryID int, entry internalexecutor.ExecutionLogEntry) error
}

// logEntryBufSize is the maximum number of log entries that are logged by the
// task execution but not yet written to the database.
const logEntryBufsize = 50

// NewLogger creates a new logger instance with the given store, job, record,
// and replacement map.
// When the log messages are serialized, any occurrence of sensitive values are
// replace with a non-sensitive value.
// Each log message is written to the store in a goroutine. The Flush method
// must be called to ensure all entries are written.
func NewLogger(store ExecutionLogEntryStore, job types.Job, recordID int, replacements map[string]string) Logger {
	oldnew := make([]string, 0, len(replacements)*2)
	for k, v := range replacements {
		oldnew = append(oldnew, k, v)
	}

	l := &logger{
		store:    store,
		job:      job,
		done:     make(chan struct{}),
		handles:  make(chan *entryHandle, logEntryBufsize),
		replacer: strings.NewReplacer(oldnew...),
		errs:     nil,
	}

	go l.writeEntries()

	return l
}

func (l *logger) Flush() error {
	close(l.handles)
	<-l.done

	l.errsMu.Lock()
	defer l.errsMu.Unlock()

	return l.errs
}

func (l *logger) Log(key string, command []string) LogEntry {
	handle := &entryHandle{
		logEntry: internalexecutor.ExecutionLogEntry{
			Key:       key,
			Command:   command,
			StartTime: time.Now(),
		},
		replacer: l.replacer,
		buf:      &bytes.Buffer{},
		done:     make(chan struct{}),
	}

	l.handles <- handle
	return handle
}

func (l *logger) writeEntries() {
	defer close(l.done)

	var wg sync.WaitGroup
	for handle := range l.handles {
		initialLogEntry := handle.CurrentLogEntry()
		entryID, err := l.store.AddExecutionLogEntry(context.Background(), l.job, initialLogEntry)
		if err != nil {
			// If there is a timeout or cancellation error we don't want to skip
			// writing these logs as users will often want to see how far something
			// progressed prior to a timeout.
			log15.Warn("Failed to upload executor log entry for job", "id", l.job.ID, "repositoryName", l.job.RepositoryName, "commit", l.job.Commit, "error", err)

			l.appendError(err)

			continue
		}
		log15.Debug("Writing log entry", "jobID", l.job.ID, "entryID", entryID, "repositoryName", l.job.RepositoryName, "commit", l.job.Commit)

		wg.Add(1)
		go func(handle *entryHandle, entryID int, initialLogEntry internalexecutor.ExecutionLogEntry) {
			defer wg.Done()

			l.syncLogEntry(handle, entryID, initialLogEntry)
		}(handle, entryID, initialLogEntry)
	}

	wg.Wait()
}

const syncLogEntryInterval = 1 * time.Second

func (l *logger) syncLogEntry(handle *entryHandle, entryID int, old internalexecutor.ExecutionLogEntry) {
	lastWrite := false

	for !lastWrite {
		select {
		case <-handle.done:
			lastWrite = true
		case <-time.After(syncLogEntryInterval):
		}

		current := handle.CurrentLogEntry()
		if !entryWasUpdated(old, current) {
			continue
		}

		logArgs := make([]any, 0, 16)
		logArgs = append(
			logArgs,
			"jobID", l.job.ID,
			"repositoryName", l.job.RepositoryName,
			"commit", l.job.Commit,
			"entryID", entryID,
			"key", current.Key,
			"outLen", len(current.Out),
		)
		if current.ExitCode != nil {
			logArgs = append(logArgs, "exitCode", *current.ExitCode)
		}
		if current.DurationMs != nil {
			logArgs = append(logArgs, "durationMs", *current.DurationMs)
		}

		log15.Debug("Updating executor log entry", logArgs...)

		if err := l.store.UpdateExecutionLogEntry(context.Background(), l.job, entryID, current); err != nil {
			logMethod := log15.Warn
			if lastWrite {
				logMethod = log15.Error
				// If lastWrite, this MUST complete for the job to be considered successful,
				// so we want to hard-fail otherwise. We store away the error.
				l.appendError(err)
			}

			logMethod(
				"Failed to update executor log entry for job",
				"jobID", l.job.ID,
				"repositoryName", l.job.RepositoryName,
				"commit", l.job.Commit,
				"entryID", entryID,
				"lastWrite", lastWrite,
				"error", err,
			)
		} else {
			old = current
		}
	}
}

func (l *logger) appendError(err error) {
	l.errsMu.Lock()
	l.errs = errors.Append(l.errs, err)
	l.errsMu.Unlock()
}

// If old didn't have exit code or duration and current does, update; we're finished.
// Otherwise, update if the log text has changed since the last write to the API.
func entryWasUpdated(old, current internalexecutor.ExecutionLogEntry) bool {
	return (current.ExitCode != nil && old.ExitCode == nil) || (current.DurationMs != nil && old.DurationMs == nil) || current.Out != old.Out
}

func redact(entry *internalexecutor.ExecutionLogEntry, replacer *strings.Replacer) {
	for i, arg := range entry.Command {
		entry.Command[i] = replacer.Replace(arg)
	}
	entry.Out = replacer.Replace(entry.Out)
}

func NewWriterLogger(w io.Writer) Logger {
	return &writerLogger{w}
}

type writerLogger struct {
	w io.Writer
}

func (*writerLogger) Flush() error { return nil }
func (l *writerLogger) Log(key string, command []string) LogEntry {
	fmt.Fprintf(l.w, "%s: %s", key, strings.Join(command, " "))
	return &writerLogEntry{w: l.w}
}

type writerLogEntry struct {
	w io.Writer
}

func (l *writerLogEntry) Write(p []byte) (n int, err error) {
	return fmt.Fprint(l.w, string(p))
}
func (*writerLogEntry) Close() error          { return nil }
func (*writerLogEntry) Finalize(exitCode int) {}
func (*writerLogEntry) CurrentLogEntry() internalexecutor.ExecutionLogEntry {
	return internalexecutor.ExecutionLogEntry{}
}
