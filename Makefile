
# Makefile for myshell

# use g++
CXX := g++

# we want executable to be myshell
TARGET := myshell

# source files
SRC := main.cpp myshell.cpp

# object files - created from compilation
OBJ := $(SRC:.cpp=.o)

# compiler flags, use C++ 20, enable warnings and include debugging
CXXFLAGS := -std=c++20 -Wall -Wextra -Wpedantic -g

# target is myshell
all: $(TARGET)


# link object files into executable
$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) $(OBJ) -o $@

# compile cpp into object files
%.o: %.cpp myshell.hpp
	$(CXX) $(CXXFLAGS) -c $< -o $@


# run if we want
run: $(TARGET)
	./$(TARGET)

# remove build artifacts
clean:
	rm -f $(TARGET) $(OBJ)
