LIBPAR3_SRC := $(wildcard src/libpar3/*.c)
LIBPAR3_OBJ := $(LIBPAR3_SRC:.c=.o)
LIBPAR3_DEP := $(LIBPAR3_SRC:.c=.d)

BLAKE3_SRC := $(wildcard src/blake3/*.c)
ifdef BLAKE3_USE_ASSEMBLY
	INTRIN := $(wildcard src/blake3/blake3_avx*.c) $(wildcard src/blake3/blake3_sse*.c)
	BLAKE3_SRC := $(filter-out $(INTRIN),$(wildcard src/blake3/*.c))
	ifeq ($(OS), Windows_NT)
		BLAKE3_ASM := $(wildcard src/blake3/*_windows_gnu.S)
	else
		BLAKE3_ASM := $(wildcard src/blake3/*_unix.S)
	endif
endif
BLAKE3_OBJ := $(BLAKE3_SRC:.c=.o)
BLAKE3_DEP := $(BLAKE3_SRC:.c=.d)

LEOPARD_SRC := $(wildcard src/leopard/*.cpp)
LEOPARD_OBJ := $(LEOPARD_SRC:.cpp=.o)
LEOPARD_DEP := $(LEOPARD_SRC:.cpp=.d)
ifdef LEOPARD_USE_NATIVE_ARCH
	LEOPARD_CXXFLAGS := -march=native -mtune=native
else
	LEOPARD_CXXFLAGS := -mssse3
endif

PAR3CMD_SRC := $(wildcard src/par3cmd/*.c)
PAR3CMD_OBJ := $(PAR3CMD_SRC:.c=.o)
PAR3CMD_DEP := $(PAR3CMD_SRC:.c=.d)

ifeq ($(OS), Windows_NT)
	PAR3PLATFORM_SRC := $(wildcard src/platform/windows/*.c)
else
	PAR3PLATFORM_SRC := $(wildcard src/platform/linux/*.c)
endif
PAR3PLATFORM_OBJ := $(PAR3PLATFORM_SRC:.c=.o)
PAR3PLATFORM_DEP := $(PAR3PLATFORM_SRC:.c=.d)

PAR3CMD_LDFLAGS := -fopenmp
PAR3CMD_LDLIBS := -lm -lstdc++
ifeq ($(OS), Windows_NT)
	PAR3CMD_LDFLAGS += -static
	EXE := .exe
endif

OBJS := $(PAR3CMD_OBJ) $(PAR3PLATFORM_OBJ) $(LIBPAR3_OBJ) $(BLAKE3_OBJ) $(LEOPARD_OBJ)
DEPS := $(PAR3CMD_DEP) $(PAR3PLATFORM_DEP) $(LIBPAR3_DEP) $(BLAKE3_DEP) $(LEOPARD_DEP)
CFLAGS := -Wall -MMD -Wno-maybe-uninitialized -Wno-unused-but-set-variable -Og -g $(CFLAGS)
CXXFLAGS := -Wall -MMD -Wno-maybe-uninitialized -Wno-unused-but-set-variable -Og -g $(CXXFLAGS)

par3: src/par3cmd/main
	mv src/par3cmd/main$(EXE) par3$(EXE)

src/par3cmd/main: LDLIBS += $(PAR3CMD_LDLIBS)
src/par3cmd/main: LDFLAGS += $(PAR3CMD_LDFLAGS)
src/par3cmd/main: $(OBJS) $(BLAKE3_ASM)

$(filter %LeopardFF8.o,$(LEOPARD_OBJ)): CXXFLAGS += $(LEOPARD_CXXFLAGS)
$(filter %LeopardCommon.o %LeopardFF16.o,$(LEOPARD_OBJ)): CXXFLAGS += $(LEOPARD_CXXFLAGS) -fopenmp
$(filter %sse2.o,$(BLAKE3_OBJ)): CFLAGS += -msse2
$(filter %sse41.o,$(BLAKE3_OBJ)): CFLAGS += -msse4.1
$(filter %avx2.o,$(BLAKE3_OBJ)): CFLAGS += -mavx2
$(filter %avx512.o,$(BLAKE3_OBJ)): CFLAGS += -mavx512f -mavx512vl

.PHONY: clean
clean:
	$(RM) par3$(EXE) $(OBJS) $(DEPS)

-include $(DEPS)
