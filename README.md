<p align="center">
  <img src="./data/icons/scalable/apps/io.github.CrimsonOrg.CrimsonText.svg" alt="Crimson Text logo" width="128" height="128">
</p>

<h1 align="center">Crimson Text</h1>
<p align="center">A text viewer and editor for Linux that renders <a href="https://daringfireball.net/projects/markdown/">Markdown</a> and many other formats.<br>
One click on the pencil edits the page, and one more shows it rendered.<br>
READMEs, logs, TODO lists and CSV tables.</p>

<p align="center">
  <a href="https://gtk.org">
    <img src="https://img.shields.io/badge/Toolkit-GTK4-4a86cf?logo=gtk&logoColor=white" alt="GTK4">
  </a>
  <a href="https://gnome.pages.gitlab.gnome.org/libadwaita/">
    <img src="https://img.shields.io/badge/Style-libadwaita-3584e4?logo=gnome&logoColor=white" alt="libadwaita">
  </a>
  <a href="https://wayland.freedesktop.org/">
    <img src="https://img.shields.io/badge/Session-Wayland-ffbc00?logo=wayland&logoColor=white" alt="Wayland">
  </a>
</p>

<p align="center">
  <a href="https://github.com/Crimson-Org/crimson-text/releases/latest"><b>Download Here!</b></a>
</p>

## Why

Other viewers start slowly, forget the window size or crowd the page with controls.\
Crimson Text remembers the window size and shows only the document, with a contents sidebar on request.

## Features

### Streaming

Documents stream in block by block, so large files and slow pictures never freeze the window.\
Pictures load from disk or the web and fade in.\
Files changed on disk reload unless you have unsaved edits.

### Markdown flavours

[GitHub](https://github.github.com/gfm/), [Obsidian](https://obsidian.md/) and [Pandoc](https://pandoc.org/) flavoured Markdown, including alerts, task lists, tables, front matter and the HTML READMEs use for centred logos and badges.

### More than Markdown

Code, scripts, logs and configuration open in a coloured editor, TODO files become tickable checklists and CSV files become tables.\
Start it with `--edit` to open files in the editor.

## Building

You need a [C compiler](https://gcc.gnu.org/), [make](https://www.gnu.org/software/make/), [pkg-config](https://www.freedesktop.org/wiki/Software/pkg-config/) and the development files for [GTK4](https://gtk.org) 4.14, [libadwaita](https://gnome.pages.gitlab.gnome.org/libadwaita/) 1.8, [GtkSourceView](https://gitlab.gnome.org/GNOME/gtksourceview) 5 and [libcurl](https://curl.se/libcurl/).\
On [openSUSE](https://www.opensuse.org/) these come from `sudo zypper install gcc make pkg-config gtk4-devel libadwaita-devel gtksourceview5-devel libcurl-devel`.

Clone it with `git clone --recurse-submodules` so the shared Crimson Core code comes along.\
`sh .crimson-build/install.sh` installs it, and it shows up in Open With.\
`sh .crimson-build/install-default.sh` also opens text, INI, Markdown, [JSON](https://www.json.org/), [TOML](https://toml.io/), XML and empty files with it, and these and shell scripts in its editor when you middle click them in [Dolphin](https://apps.kde.org/dolphin/).\
`sh .crimson-build/uninstall.sh` removes it.\
`sh .crimson-build/run.sh` builds and starts it and `sh .crimson-build/archive.sh` writes a tar.gz to `dist/`.\
`sh .crimson-build/appimage.sh` writes an [AppImage](https://appimage.org/) to `dist/` on [Arch Linux](https://archlinux.org/).
