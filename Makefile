CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=gnu11 -Isrc
LDLIBS  = -lpthread
BIN     = bin

all: $(BIN)/etapa1 $(BIN)/etapa2 $(BIN)/intertarefas $(BIN)/test_huffman

$(BIN):
	mkdir -p $(BIN)

$(BIN)/etapa1: src/etapa1_tarefa.c src/huffman.c src/huffman.h src/common.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ src/etapa1_tarefa.c src/huffman.c $(LDLIBS)

$(BIN)/etapa2: src/etapa2_comportamental.c src/huffman.c src/huffman.h src/common.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ src/etapa2_comportamental.c src/huffman.c $(LDLIBS)

$(BIN)/intertarefas: src/intertarefas.c src/huffman.c src/huffman.h src/common.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ src/intertarefas.c src/huffman.c $(LDLIBS)

# Casos de teste (versao otimizada, para medir tempo de cada caso)
$(BIN)/test_huffman: tests/test_huffman.c src/huffman.c src/huffman.h src/common.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ tests/test_huffman.c src/huffman.c $(LDLIBS)

# Experimento A: cobertura com gcov (-O0 --coverage)
coverage: | $(BIN)
	rm -rf cov && mkdir -p cov
	$(CC) -O0 --coverage -std=gnu11 -Isrc -o cov/test_huffman_cov tests/test_huffman.c src/huffman.c $(LDLIBS)

clean:
	rm -rf $(BIN) cov *.gcda *.gcno *.gcov

.PHONY: all coverage clean
