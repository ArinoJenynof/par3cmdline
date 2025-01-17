CFLAGS ?= -Wall -O3 -s
CXXFLAGS ?= -Wall -O3 -s
LDFLAGS ?= -Wl,-Bstatic

PAR3_SRC := $(wildcard src/*.c)
PAR3_OBJ := $(PAR3_SRC:.c=.o)

BLAKE3_SRC := $(wildcard src/blake3/*.c)
BLAKE3_OBJ := $(BLAKE3_SRC:.c=.o)
BLAKE3_ASM := $(wildcard src/blake3/*.S)

LEOPARD_SRC := $(wildcard src/leopard/*.cpp)
LEOPARD_OBJ := $(LEOPARD_SRC:.cpp=.o)

par3: $(PAR3_OBJ) $(BLAKE3_OBJ) $(BLAKE3_ASM) $(LEOPARD_OBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fopenmp $(LDFLAGS) $^ $(LDLIBS) -lstdc++ -o $@

$(PAR3_OBJ): %.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wno-error=incompatible-pointer-types -c $^ -o $@

$(LEOPARD_OBJ): %.o: %.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -march=native -fopenmp -c $^ -o $@

PHONY: clean
clean:
	$(RM) par3 $(PAR3_OBJ) $(BLAKE3_OBJ) $(LEOPARD_OBJ)
