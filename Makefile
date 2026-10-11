CXX      := g++
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra
AS       := nasm
ASFLAGS  := -f elf64
LIBS     := -lSDL2
TARGET   := Disnakector
BUILD    := build

all: $(TARGET)

$(TARGET): $(BUILD)/main.o $(BUILD)/logic.o
	$(CXX) $^ $(LIBS) -o $@

$(BUILD)/main.o: src/main.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/logic.o: src/logic.asm | $(BUILD)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD) $(TARGET)

run: $(TARGET)
	./$(TARGET)

.PHONY: all clean run
