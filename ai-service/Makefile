SHELL := /bin/bash
NODE := $(shell bash ../scripts/find_ai_node.sh)
export PATH := $(dir $(NODE)):$(PATH)
NPM := $(NODE) $(dir $(NODE))../lib/node_modules/npm/bin/npm-cli.js

.PHONY: install lint build test eval eval-real run clean
install:
	$(NPM) ci --ignore-scripts --no-audit --no-fund
lint:
	$(NPM) run lint
build:
	$(NPM) run build
test:
	$(NPM) test
eval:
	$(NPM) run eval
eval-real:
	$(NPM) run eval:real
run: build
	$(NPM) start
clean:
	rm -rf dist
