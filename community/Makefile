.PHONY: community.wasm

src = $(wildcard *.cpp)
obj = $(src:.cpp=.wasm) 
OPTS = -D'__TOKEN_ACCOUNT__=cambiatus.tk' -D'__BACKEND_ACCOUNT__=cambiatus'

community.wasm: $(src)
	eosio-cpp $(OPTS) -o $@ -abigen -R ./ricardian $^

clean:
	rm $(obj)
