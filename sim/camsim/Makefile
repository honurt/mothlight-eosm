# camsim: the real moth_ui.c against a fake EOS M. `make -C sim/camsim test`
UI      = ../../modules/moth_ui
FONTS  ?= $(firstword $(wildcard ../../work/cropmood/data/fonts ../../work/upstream/data/fonts))
CFLAGS ?= -DMOTH_SIM -O1 -g -std=gnu99 -Wall -Wno-unused-parameter -Wno-unused-function -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie -Wno-int-to-pointer-cast
PURE    = $(UI)/themes.c $(UI)/panel.c $(UI)/ui_gfx.c $(UI)/ui_text.c $(UI)/budget.c $(UI)/profiles.c

camsim: camsim.c camsim_render.c ../simlib.c fake/*.h fake/module_keys.h $(UI)/*.c $(UI)/*.h
	$(CC) $(CFLAGS) -Ifake -o $@ camsim.c camsim_render.c ../simlib.c $(PURE) -lpthread

fake/module_keys.h: ../module_keys.h
	cp $< $@
../module_keys.h:
	$(MAKE) -C .. module_keys.h

test: camsim
	@test -n "$(FONTS)" || { echo "no fonts: run scripts/fetch.sh first"; exit 1; }
	./camsim $(FONTS)

clean:
	rm -f camsim fake/module_keys.h
.PHONY: test clean
