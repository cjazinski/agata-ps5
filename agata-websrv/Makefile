# agata-ps5 payload build
PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

ELF := agata_ps5_websrv.elf

CFLAGS := -Wall -Werror -g

# embed git commit + build date so the UI can show exactly what's running
BUILD_ID := $(shell git rev-parse --short HEAD 2>/dev/null || echo nogit)-$(shell date +%Y%m%d)
CFLAGS += -DBUILD_ID=\"$(BUILD_ID)\"
export BUILD_ID

all: $(ELF)

ui.h: ui.html embed_ui.py
	python3 embed_ui.py ui.html ui.h

LZMA_SRC := lzma/C/7zAlloc.c lzma/C/7zArcIn.c lzma/C/7zBuf.c lzma/C/7zBuf2.c \
            lzma/C/7zCrc.c lzma/C/7zCrcOpt.c lzma/C/7zDec.c lzma/C/7zFile.c \
            lzma/C/7zStream.c lzma/C/Aes.c lzma/C/Alloc.c \
            lzma/C/Bra.c lzma/C/Bra86.c lzma/C/BraIA64.c lzma/C/CpuArch.c \
            lzma/C/Delta.c lzma/C/LzmaDec.c lzma/C/Lzma2Dec.c lzma/C/Ppmd7.c \
            lzma/C/Ppmd7Dec.c lzma/C/Sha256.c lzma/C/MtDec.c lzma/C/Threads.c \
            lzma/C/Xz.c lzma/C/XzCrc64.c lzma/C/XzCrc64Opt.c lzma/C/XzDec.c lzma/C/Bcj2.c lzma/C/Sha256Opt.c

LZMA_SRC += lzma/C/AesOpt.c

LZMA_OBJ := $(LZMA_SRC:.c=.o)

# intrinsics files need explicit target attrs on this toolchain
lzma/C/AesOpt.o: CFLAGS += -maes -mvaes -mavx -mavx2
lzma/C/Sha256Opt.o: CFLAGS += -msha -mssse3 -mavx

$(ELF): main.c seven_zip.c http.h ui.h $(LZMA_OBJ)
	$(CC) $(CFLAGS) -o $@ main.c seven_zip.c $(LZMA_OBJ) -lpthread

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(ELF) $(LZMA_OBJ)

test: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^
lzma/C/Sha256Opt.o: CFLAGS += -msha -mssse3 -mavx
