.PHONY: all clean

all: test_dma

test_dma:
	@./build.sh

clean:
	@rm -f *.o test_dma
