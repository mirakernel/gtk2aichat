# Repository Guidelines

## Project Structure & Module Organization

- `src/main.c` contains the GTK2 interface, chat model, persistence, Markdown rendering, and OpenAI/Ollama HTTP clients.
- `Makefile` defines the native C build and required `pkg-config` packages.
- `gtk2aichat.desktop` is the desktop launcher template.
- `README.md` documents features, dependencies, installation, and configuration.

There is currently no dedicated test directory or asset bundle. If the application is split into modules, place public headers in `src/` beside their implementations and group code by responsibility, such as `storage.c`, `provider.c`, and `ui.c`.

## Build, Test, and Development Commands

Install development packages for GTK+ 2, GLib threads, libcurl, json-c, and pkg-config.

```sh
make
```

Builds the optimized `gtk2aichat` executable with GCC-compatible warnings enabled.

```sh
./gtk2aichat
make clean
desktop-file-validate gtk2aichat.desktop
```

These commands run the application, remove build output, and validate the launcher. Before submitting changes, run `make clean && make` and `git diff --check`.

## Coding Style & Naming Conventions

Use C with four-space indentation and no tabs in source files. Prefer GLib types and ownership helpers when interacting with GTK. Use `snake_case` for functions and variables, `PascalCase` for structs, and descriptive callback names beginning with `on_`.

Keep all GTK widget access on the main thread. Network work belongs in worker threads; deliver UI changes through `g_idle_add()`. Preserve compatibility with GTK+ 2 and 32-bit Gentoo systems. Avoid adding heavy dependencies without a clear benefit.

## Testing Guidelines

No automated test framework is configured yet. Every change must compile without project warnings. Manually verify affected UI behavior, ordinary and temporary chat persistence, both providers, streaming, and UTF-8 text. New parsers or storage modules should include focused tests under `tests/` with names such as `test_markdown.c`.

## Commit & Pull Request Guidelines

Follow the existing concise Conventional Commit pattern:

```text
feat: add chat deletion
fix: apply chat text tags correctly
```

Keep commits scoped to one behavior. Pull requests should explain the user-visible result, list verification commands, mention 32-bit/GTK2 implications, and include screenshots for interface changes. Link relevant issues when available.

## Security & Configuration

Never commit API keys or files from `~/.config/gtk2aichat/`. Prefer `OPENAI_API_KEY` for local development. Preserve restrictive permissions and atomic writes for saved settings and chat history.
