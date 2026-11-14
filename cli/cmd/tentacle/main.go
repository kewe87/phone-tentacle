// tentacle sees and drives an iPhone through a Tentacle board, over Wi-Fi.
package main

import (
	"os"

	"github.com/kewe87/phone-tentacle/cli/internal/app"
)

func main() { os.Exit(app.Main(os.Args[1:])) }
