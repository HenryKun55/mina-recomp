#!/bin/sh
# Closes this port's Vita3K session (by pid: a pattern on the command line would match the caller too)
for p in $(pgrep -f "MacOS/Vita3K -c .*vita3k-mina"); do kill -9 "$p"; done
exit 0
