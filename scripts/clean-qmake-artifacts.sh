#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

find "$repo_root" \
  \( \
    -path "$repo_root/.git" -o \
    -path "$repo_root/.claude" -o \
    -path "$repo_root/.qtc_clangd" -o \
    -path "$repo_root/.qtcreator" -o \
    -path "$repo_root/.superpowers" -o \
    -path "$repo_root/build" -o \
    -path "$repo_root/coverage" \
  \) -prune \
  -o \( \
    -name 'Makefile' -o \
    -name 'Makefile.*' -o \
    -name '*.o' -o \
    -name '.qmake.stash' -o \
    -name '.qmake.cache' -o \
    -name '*.moc' -o \
    -name 'moc_*.cpp' -o \
    -name 'moc_*.h' -o \
    -name 'qrc_*.cpp' -o \
    -name 'ui_*.h' -o \
    -name 'rep_*_replica.h' -o \
    -name 'moc_rep_*_replica.cpp' \
  \) -type f -print -delete

find "$repo_root" \
  \( \
    -path "$repo_root/.git" -o \
    -path "$repo_root/.claude" -o \
    -path "$repo_root/.qtc_clangd" -o \
    -path "$repo_root/.qtcreator" -o \
    -path "$repo_root/.superpowers" -o \
    -path "$repo_root/build" -o \
    -path "$repo_root/coverage" \
  \) -prune \
  -o \( -name 'FastECU.app' -o -name 'dist' \) -type d -print -exec rm -rf {} +
