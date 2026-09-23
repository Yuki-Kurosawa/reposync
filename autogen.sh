#!/bin/sh
# Generate ./configure and the Makefile.in templates (run once after a
# fresh checkout, or whenever configure.ac / Makefile.am change).
autoreconf -i
