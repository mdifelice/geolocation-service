COMPILER=gcc
FLAGS=-pthread -lm
OUTPUT=bin/server

all:
	$(COMPILER) src/main.c -o $(OUTPUT) $(FLAGS)
