package proto

import "context"

type logKey struct{}

// WithLog returns a context whose Logf calls go to f: the verbose trace of one request (discovery, frames, timings).
func WithLog(ctx context.Context, f func(format string, args ...any)) context.Context {
	if f == nil {
		return ctx
	}
	return context.WithValue(ctx, logKey{}, f)
}

// Logf writes a verbose line if ctx carries a log (WithLog); otherwise it does nothing.
func Logf(ctx context.Context, format string, args ...any) {
	if f, ok := ctx.Value(logKey{}).(func(string, ...any)); ok {
		f(format, args...)
	}
}
