all: simple persistent pipelined

simple: SimpleServer.cc
	clang++ -std=c++17 SimpleServer.cc -o SimpleServer

persistent: PersistentServer.cc
	clang++ -std=c++17 PersistentServer.cc -o PersistentServer

pipelined: PipelinedServer.cc
	clang++ -std=c++17 PipelinedServer.cc -o PipelinedServer

.PHONY: clean

clean:
	-rm SimpleServer PersistentServer PipelinedServer
