CC = gcc
LD = ld

CFLAGS = \
	-m32 \
	-ffreestanding \
	-fno-pie \
	-fno-stack-protector \
	-fno-builtin \
	-O0 \
	-Wall \
	-Wextra

LDFLAGS = \
	-m elf_i386 \
	-T linker.ld

OBJS = boot.o dma_test.o

all: dma_test.elf

boot.o: boot.S
	$(CC) -m32 -c boot.S -o boot.o

dma_test.o: dma_test.c
	$(CC) $(CFLAGS) -c dma_test.c -o dma_test.o

dma_test.elf: $(OBJS)
	$(LD) $(LDFLAGS) $(OBJS) -o dma_test.elf

clean:
	rm -f *.o *.elf
