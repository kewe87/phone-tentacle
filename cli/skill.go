// Package cli holds files embedded in the tentacle binary.
package cli

import _ "embed"

// Skill is SKILL.md, installed for agents by `tentacle skill install`.
//
//go:embed SKILL.md
var Skill string
