# Makefile — Trabalho Final de ATR
#   make        -> gera ./logistica e ./robo
#   make clean  -> apaga executáveis e objetos

CC     = gcc
CFLAGS = -std=c11 -D_GNU_SOURCE -Wall -Wextra -pthread -Iinclude
LIBS   = -pthread -lrt
LIBS_LOGISTICA = $(LIBS) -lmosquitto   # só o processo central usa MQTT

OBJ_LOGISTICA = src/main.o src/memoria.o src/coletor.o src/eventos.o src/gestor.o \
                src/console.o src/sim_stub.o src/mapa.o src/organizador.o \
                src/ponte_mqtt.o
OBJ_ROBO      = src/robo_main.o src/memoria.o src/eventos.o src/status.o \
                src/navegacao.o src/mapa.o

all: logistica robo

logistica: $(OBJ_LOGISTICA)
	$(CC) -o $@ $(OBJ_LOGISTICA) $(LIBS_LOGISTICA)

robo: $(OBJ_ROBO)
	$(CC) -o $@ $(OBJ_ROBO) $(LIBS)

src/%.o: src/%.c include/comum.h include/mapa.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f logistica robo src/*.o

.PHONY: all clean
