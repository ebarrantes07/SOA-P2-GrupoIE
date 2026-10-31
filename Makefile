# Makefile inicial del Proyecto 2 (MC6004).
# Se completa a medida que se agregan módulos; ver docs/decisiones_diseno.md.

# make predefine CC = cc, así que `CC ?= gcc` nunca tendría efecto. Se usa
# gcc salvo que el usuario elija otro (make CC=clang o variable de entorno).
ifeq ($(origin CC),default)
CC       := gcc
endif
CPPFLAGS := -Iinclude -D_POSIX_C_SOURCE=200809L
CFLAGS   := -std=c17 -Wall -Wextra -Wpedantic -Werror -pthread
LDFLAGS  := -pthread
LDLIBS   := -lm
# -lrt solo es necesario en glibc antiguas para shm_open; no existe en macOS.
ifeq ($(shell uname -s),Linux)
LDLIBS   += -lrt
endif

# Ejecutables: cada uno tiene su main en src/<nombre>.c.
BINS     := creator producer consumer finisher verify
MAINS    := $(addprefix src/,$(addsuffix .c,$(BINS)))

# Módulos comunes: todo src/*.c que no sea un main.
COMMON   := $(filter-out $(MAINS),$(wildcard src/*.c))
OBJDIR   := build
COMMON_O := $(patsubst src/%.c,$(OBJDIR)/%.o,$(COMMON))

# Solo se construyen los ejecutables cuyo main ya existe.
PRESENT  := $(patsubst src/%.c,%,$(wildcard $(MAINS)))

# Pruebas unitarias por módulo: cada tests/test_*.c es su propio binario,
# enlazado contra los mismos módulos comunes. Los de integración de punta a
# punta (#19-#22) corren aparte, sobre los ejecutables de BINS.
TEST_SRC := $(wildcard tests/test_*.c)
TEST_BIN := $(patsubst tests/%.c,$(OBJDIR)/%,$(TEST_SRC))

# ---- Sanitizers (#23) ---------------------------------------------------

# ASan + UBSan sobre la suite completa.
SANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -g

# UBSan por defecto IMPRIME Y SIGUE, asi que una corrida con comportamiento
# indefinido reportado terminaria en exito y el objetivo no serviria para el
# criterio "cero errores reportados" del #23. halt_on_error lo vuelve fatal,
# como ASan ya lo es. detect_leaks va explicito aunque sea el default en
# Linux: el objetivo declara que tambien busca fugas.
SANENV := ASAN_OPTIONS=detect_leaks=1 \
          UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1

.PHONY: all test test-san clean

all: $(PRESENT)
	@$(if $(PRESENT),true,echo "Aún no hay ejecutables en src/; nada que compilar.")

$(PRESENT): %: $(OBJDIR)/%.o $(COMMON_O)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OBJDIR)/%.o: src/%.c | $(OBJDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(OBJDIR)/test_%.o: tests/test_%.c | $(OBJDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(OBJDIR)/test_%: $(OBJDIR)/test_%.o $(COMMON_O)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OBJDIR):
	mkdir -p $@

test: all $(TEST_BIN)
	@if [ -z "$(TEST_BIN)" ]; then \
		echo "make test: aún no hay pruebas en tests/."; \
	else \
		fail=0; \
		for t in $(TEST_BIN); do \
			echo "-- $$t --"; \
			$$t || fail=1; \
		done; \
		exit $$fail; \
	fi

# Limpia antes y despues a proposito, en vez de convivir con la compilacion
# normal. Los objetos sanitizados comparten $(OBJDIR) y los ejecutables de
# BINS viven en la raiz, asi que al quedar mas nuevos que los .o normales
# make los daria por actualizados: un `make test` posterior seguiria
# corriendo los binarios instrumentados sin avisar. El `clean` final deja el
# arbol como estaba, y preserva el codigo de salida de la suite.
test-san:
	$(MAKE) clean
	$(SANENV) $(MAKE) test \
	    CFLAGS="$(CFLAGS) $(SANFLAGS)" \
	    LDFLAGS="$(LDFLAGS) $(SANFLAGS)"; \
	  rc=$$?; $(MAKE) clean; exit $$rc

clean:
	rm -rf $(OBJDIR) $(BINS)

-include $(wildcard $(OBJDIR)/*.d)
