package app

import (
	"os"
	"path/filepath"

	cli "github.com/kewe87/phone-tentacle/cli"
	"github.com/kewe87/phone-tentacle/cli/internal/daemon"
)

// skillInstall writes the skill for Claude Code and says how to set up Codex.
func (a *App) skillInstall() int {
	home, err := os.UserHomeDir()
	if err != nil {
		return a.fail(err)
	}
	dir := filepath.Join(home, ".claude", "skills", "tentacle")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return a.fail(err)
	}
	path := filepath.Join(dir, "SKILL.md")
	if err := os.WriteFile(path, []byte(cli.Skill), 0o644); err != nil {
		return a.fail(err)
	}
	return a.print(daemon.Response{Status: "ok", Data: map[string]any{
		"claude": map[string]string{"skill": path, "mcp": "claude mcp add tentacle -- tentacle mcp"},
		"codex": map[string]string{"mcp": "codex mcp add tentacle -- tentacle mcp",
			"skill": "copy " + path + " to ~/.codex/skills/tentacle/SKILL.md, or paste it into AGENTS.md"},
	}})
}
