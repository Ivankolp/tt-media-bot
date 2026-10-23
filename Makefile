CXX = g++
CXXFLAGS = -std=c++17 -O3 -Wall -Wextra -Iinclude
LDFLAGS = -lstdc++ -Llib -lTeamTalk5 -lcurl -lpthread -ldl -Wl,-rpath,'$$ORIGIN/lib'

SRCS = src/main.cpp src/bot.cpp src/audio_streamer.cpp src/youtube_resolver.cpp src/playlist_manager.cpp
OBJS = $(SRCS:.cpp=.o)
TARGET = tt-media-bot

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) $(LDFLAGS) -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET) bin_test src/test_link.o

.PHONY: all clean
