TARGET = client 

all: $(TARGET)
	
$(TARGET):
	g++ -std=c++14 -fpermissive -w client.cpp -lpthread -o client -lncurses


clean:
	rm $(TARGET)
