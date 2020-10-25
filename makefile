
CC=clang++
CFLAGS=--std=c++11

tsamgroup120: server.cpp recipient.hpp
	$(CC) $(CFLAGS) server.cpp -o tsamgroup120
