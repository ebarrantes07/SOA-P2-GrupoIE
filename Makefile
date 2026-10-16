# Makefile inicial del Proyecto 2 (MC6004).
# Se completa a medida que se agregan módulos; ver docs/decisiones_diseno.md.

CC       ?= gcc
CPPFLAGS := -Iinclude -D_POSIX_C_SOURCE=200809L
CFLAGS   := -std=c17 -Wall -Wextra -Wpedantic -Werror -pthread
LDFLAGS  := -pthread
LDLIBS   := -lm
# -lrt solo es necesario en glibc antiguas para shm_open; no existe en macOS.
ifeq ($(shell uname -s),Linux)
LDLIBS   += -lrt
endif

# Ejecutables: cada uno tiene su main en src/<nombre>.c.
BINS     := creator producer consumer finisher
MAINS    := $(addprefix src/,$(addsuffix .c,$(BINS)))

# Módulos comunes: todo src/*.c que no sea un main.
COMMON   := $(filter-out $(MAINS),$(wildcard src/*.c))
OBJDIR   := build
COMMON_O := $(patsubst src/%.c,$(OBJDIR)/%.o,$(COMMON))

# Solo se construyen los ejecutables cuyo main ya existe.
PRESENT  := $(patsubst src/%.c,%,$(wildcard $(MAINS)))

.PHONY: all test clean

all: $(PRESENT)
	@$(if $(PRESENT),true,echo "Aún no hay ejecutables en src/; nada que compilar.")

$(PRESENT): %: $(OBJDIR)/%.o $(COMMON_O)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OBJDIR)/%.o: src/%.c | $(OBJDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(OBJDIR):
	mkdir -p $@

# Placeholder: las pruebas automatizadas se agregan en #19, #20, #21 y #22.
test: all
	@echo "make test: aún no hay pruebas definidas."

clean:
	rm -rf $(OBJDIR) $(BINS)

-include $(wildcard $(OBJDIR)/*.d)
