# PC simulator and tests for the Mothlight UI.
#   make -C sim            build the simulator
#   make -C sim preview    run it and render out/preview/panel.gif (needs an engine in work/ for the fonts)
#   make -C sim test       unit tests for the budget and profile parser
CC      ?= cc
CFLAGS  ?= -O2 -std=gnu99 -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -g
UI      = ../modules/moth_ui
SRC     = $(UI)/themes.c $(UI)/panel.c $(UI)/ui_gfx.c $(UI)/ui_text.c $(UI)/budget.c $(UI)/profiles.c
FONTS  ?= $(firstword $(wildcard ../work/cropmood/data/fonts ../work/upstream/data/fonts))
OUT    ?= ../out/preview

all: sim e2e test_budget test_keys

sim: sim.c simlib.c simlib.h $(SRC) $(UI)/*.h
	$(CC) $(CFLAGS) -o $@ sim.c simlib.c $(SRC)

e2e: e2e.c simlib.c simlib.h $(SRC) $(UI)/*.h
	$(CC) $(CFLAGS) -o $@ e2e.c simlib.c $(SRC)

test_budget: ../tests/test_budget.c $(UI)/budget.c $(UI)/profiles.c $(UI)/*.h
	$(CC) $(CFLAGS) -o $@ ../tests/test_budget.c $(UI)/budget.c $(UI)/profiles.c

MODULE_H ?= $(firstword $(wildcard ../work/cropmood/src/module.h ../work/upstream/src/module.h))

module_keys.h: $(MODULE_H)
	@test -n "$(MODULE_H)" || { echo "no engine: run scripts/fetch.sh first"; exit 1; }
	grep '^#define MODULE_KEY_' $(MODULE_H) > $@

test_keys: ../tests/test_keys.c $(UI)/keymap.h $(UI)/panel.h module_keys.h
	$(CC) $(CFLAGS) -I. -o $@ ../tests/test_keys.c

test: test_budget test_keys
	./test_budget
	./test_keys
	$(MAKE) -C camsim test

preview: sim
	@test -n "$(FONTS)" || { echo "no fonts: run scripts/fetch.sh first"; exit 1; }
	rm -rf $(OUT) && mkdir -p $(OUT)
	for t in 0 1 2 3 4 5 6 7 8 9 10; do mkdir -p $(OUT)/frames$$t && ./sim $(FONTS) $(OUT)/frames$$t $$t > /dev/null || exit 1; done
	./sim $(FONTS) $(OUT)/frames0 0
	python3 render.py $(OUT) $(OUT)
	cp $(OUT)/frames0/stats.txt $(OUT)/stats.txt

e2e-video: e2e
	@test -n "$(FONTS)" || { echo "no fonts: run scripts/fetch.sh first"; exit 1; }
	rm -rf $(OUT)/e2e && mkdir -p $(OUT)/e2e
	./e2e $(FONTS) $(OUT)/e2e
	python3 render_e2e.py $(OUT)/e2e $(OUT)

clean:
	rm -f sim e2e test_budget test_keys module_keys.h
