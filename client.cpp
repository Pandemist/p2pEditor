//g++ -std=c++14 -fpermissive -w client.cpp -lpthread -o client -lncurses

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <string>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <fstream>
#include <net/if.h>
#include <map>
#include <pthread.h>
#include <vector>
#include <mutex>
#include <iostream>
#include <time.h>
#include <curses.h>
#include <ifaddrs.h>

/*
Definition NachrichtenTypen:
NEW 	Verbindungsaufbau
MSG		Änderungsnachricht für Content
PER 	Daten eines Peers versenden
CRR 	Aktueller stand des Pads. Muss abgearbeitet werden, bevor MSG verwendet werden
RDY		Mitteilen, dass der aktuelle Stand der Daten übertragen wurde

*/

using namespace std;

bool debug;
bool saveFlag;

struct peer{
	int peerFd;
	string peerAdresse;
	int peerPort;
	int clock;
};
int myClock;
mutex myClockMutex;

vector<peer> peers;
mutex peerMutex;

mutex logfileMutex;

vector<vector<char>> content;
mutex contentMutex;

mutex rdLock;
mutex newLock;
mutex printMutex;
mutex screenMutex;

mutex cursorMuxtex;
mutex globalShiftMutex;
int globalShiftX;
int globalShiftY;

string trenner;

string myIp;
int rcvFd;
unsigned short local_port;
string local_ip;
pthread_t rcvThread;

struct sockaddr_in rcv_addr, host_addr, newCon_addr;

socklen_t sock_t = sizeof(struct sockaddr_in);

// Schreibt text in das LogFile
void write_text_to_log_file( const std::string &text ) {
	if(!debug) {
		return;
	}
	logfileMutex.lock();
    std::ofstream log_file(
        "log_file.txt", std::ios_base::out | std::ios_base::app );
    log_file << "[" << local_port << "] " << text << std::endl;
    logfileMutex.unlock();
}

// Gibt die Ip Adresse für einen File Descriptor zurück
string getIpByFd(int fd) {
	peerMutex.lock();
	for(int i = 0; i < peers.size(); i++) {
		if(peers[i].peerFd == fd) {
			peerMutex.unlock();
			return peers[i].peerAdresse;
		}
	}
	peerMutex.unlock();
	return "";
}

// Gibt den Port für einen File Descriptor zurück
int getPortByFd(int fd) {
	peerMutex.lock();
	for(int i = 0; i < peers.size(); i++) {
		if(peers[i].peerFd == fd) {
			peerMutex.unlock();
			return peers[i].peerPort;
		}
	}
	peerMutex.unlock();
	return -1;
}

// beendet das ncurses window
void quit() {
	endwin();
}

// beendet den clienten und schreibt eine Nachricht ins LogFile
void die(const char* msg) {
	quit();
	write_text_to_log_file(msg);
	fputs(msg, stderr);
	putc('\n', stderr);
	exit(-1);
}

// Sendet die Nachricht msg an den Socket socket
bool sendThis(int socket, string msg) {
	write_text_to_log_file("MSG in Send Queue: " + msg + " - " + std::to_string(socket));
	msg = msg + trenner;
	if(send(socket, (char *)(msg.c_str()), size_t(msg.size()), 0) < 0) {
		write_text_to_log_file("Errno: " + std::to_string(errno) + " - " + string(strerror(errno)));
		write_text_to_log_file("ERROR: Es konnte keine Nachrichten gesendet werden.");
		return false;
	}
	write_text_to_log_file("MSG Sended");
	return true;
}

// Sendet eine Nachricht an alle bekannten Peers
bool sendThisToAll(string msg) {
	bool ret = true;
	peerMutex.lock();
	write_text_to_log_file(msg);
	myClockMutex.lock();
	myClock++;
	myClockMutex.unlock();
	for(int i = 0; i < peers.size(); i++) {
		ret = ret and sendThis(peers[i].peerFd, msg);
	}
	peerMutex.unlock();
	return ret;
}

// aktualisiert die header Informationen
void refreshHeader(string _local_ip, int _local_port, int _peers_lvl) {
	_peers_lvl += 1;
	int cPosX;
	int cPosY;
	int w;
	int h;
	screenMutex.lock();
	getyx(stdscr, cPosY, cPosX);
	getmaxyx(stdscr, h, w);
	color_set(1, 0);
	for(int i = 0; i < w; i++) {
		mvaddch(2, i, ' ');
	}
	color_set(0, 0);
	screenMutex.unlock();

// Set Data
	string ipText;
	ipText.append("IP Adress: ").append(_local_ip);
	int ipText_l = ipText.length();

	string portText;
	portText.append("Local Port: ").append(std::to_string(_local_port));
	int portText_l = portText.length();

	string peerText;
	peerText.append("Anzahl Peers: ").append(std::to_string(_peers_lvl));
	int peerText_l = peerText.length();

	string verbindung;
	verbindung.append("Verbindung: ");
	string verbindungStatus = "[OK]";
	int ident = 0;

	screenMutex.lock();
	mvaddstr(0, ident, (char*) ipText.c_str());
	mvaddstr(1, ident, (char*) portText.c_str());
	ident = max(ipText_l, portText_l)+1;
	mvaddch(0, ident, '|');
	mvaddch(1, ident, '|');
	ident += 2;
	mvaddstr(0, ident, (char*) peerText.c_str());
	mvaddstr(1, ident, (char*) verbindung.c_str());
	color_set(2, 0);
	mvaddstr(1, ident+verbindung.length(), (char*) verbindungStatus.c_str());
	color_set(0, 0);
	ident += max(peerText_l, (int) (verbindung.length()+verbindungStatus.length()));
	mvaddch(0, ident, '|');
	mvaddch(1, ident, '|');

	move(cPosY, cPosX);
	screenMutex.unlock();
}

// aktualisiert den Footer
void refreshFooter() {
	int cPosX;
	int cPosY;
	int w;
	int h;
	screenMutex.lock();

	getyx(stdscr, cPosY, cPosX);
	getmaxyx(stdscr, h, w);

	color_set(1, 0);
	for(int i = 0; i < w; i++) {
		mvaddch(h-1, i, ' ');
	}
	color_set(0, 0);

	mvaddstr(h-1, 0, "F6");
	color_set(1, 0);
	mvaddstr(h-1, 3, "Save");
	color_set(0, 0);

	mvaddstr(h-1, 8, "F10");
	color_set(1, 0);
	mvaddstr(h-1, 12, "Quit");
	color_set(0, 0);
	move(cPosY, cPosX);
	screenMutex.unlock();
}

// Leert den Bildschirm vollständig
void clearScreen() {
	// Prepare Display
	int w;
	int h;
	screenMutex.lock();
	getmaxyx(stdscr, h, w);
	for(int i = 0; i < w; i++) {
		for(int j = 0; j < h; j++) {
			mvaddch(j, i, ' ');
		}
	}
	screenMutex.unlock();
}

// Speichert den geschriebenen Text in eine Datei
void save() {
	// Prepare Display
	printMutex.lock();
	contentMutex.lock();
    ofstream log_file("output.txt", ios_base::out | ios_base::app);
    for(int i = 0; i < content.size(); i++) {
    	for(int j = 0; j < content[i].size();j++) {
    		log_file << content[j][i];
    	}
    	log_file << endl;
    }
    contentMutex.unlock();
    printMutex.unlock();
}

// Rendert die Contentmatrix auf dem Bildschirm
void renderScreen() {
	int w;
	int h;
	int gX = 0;
	int gY = 0;

	int cPosX;
	int cPosY;
	
	globalShiftMutex.lock();
	gX = globalShiftX;
	gY = globalShiftY;
	globalShiftMutex.unlock();

	screenMutex.lock();
	contentMutex.lock();

	getmaxyx(stdscr, h, w);
	getyx(stdscr, cPosY, cPosX);
	write_text_to_log_file("Run refresh!");
	for(int i = 0; i < w; i++) {
		for(int j = 0; j < h-4; j++) {
			mvaddch(j + 3, i, content[i+gX][j+gY]);
		}
	}
	write_text_to_log_file("refresh completed!");
	contentMutex.unlock();
	move(cPosY, cPosX);
	refresh();
	screenMutex.unlock();
}

// aktualisiert ein einziges Symbol
void renderSingleScreen(int x, int y) {
	int w;
	int h;
	int gX = 0;
	int gY = 0;

	int xP;
	int yP;

	int cPosX;
	int cPosY;
	char c;
	
	xP = x;
	yP = y;

	getmaxyx(stdscr, h, w);

	if((x >= w) || (y >= h)) {
		return;
	}

	write_text_to_log_file("Zeichen updaten bei: " + std::to_string(xP) + " - " + std::to_string(yP));

	contentMutex.lock();
	c = content[xP][yP];
	contentMutex.unlock();

	screenMutex.lock();
	getyx(stdscr, cPosY, cPosX);

	write_text_to_log_file("Update: " + std::to_string(x) + "-" + std::to_string(y+3) + "~" + std::to_string(c));
	//mvaddch(3, x, 'c');
	move(y + 3, x);
	sleep(0.1);
	addch(c);
	move(cPosY, cPosX);
	sleep(0.1);
	refresh();
	screenMutex.unlock();
}

// Updated die position des Curors und der Contentmatrix
void updateCursor(int xMov, int yMov) {
	int w;
	int h;
	getmaxyx(stdscr, h, w);

// Cursor Position anpassen
	cursorMuxtex.lock();
	int cPosX;
	int cPosY;
	int gShiftTmpX = 0;
	int gShiftTmpY = 0;
	getyx(stdscr, cPosY, cPosX);

	if((cPosX + xMov) > w-1) {
		gShiftTmpX = xMov;
		xMov = 0;
	}

	if((cPosX + xMov) < 0) {
		gShiftTmpX = xMov;
		xMov = 0;
	}

	if((cPosY + yMov) > h-2) {
		gShiftTmpY = yMov;
		yMov = 0;
	}

	if((cPosY + yMov) < 3) {
		gShiftTmpY = yMov;
		yMov = 0;
	}

	move(cPosY + yMov, cPosX + xMov);
	cursorMuxtex.unlock();

	int oldX;
	int oldY;
	bool shifted;

// Global shift verschieben
	globalShiftMutex.lock();
	oldX = globalShiftX;
	oldY = globalShiftY;
	globalShiftX = max(0, globalShiftX + gShiftTmpX);
	globalShiftY = max(0, globalShiftY + gShiftTmpY);
	shifted = ((globalShiftX - oldX) != 0) or ((globalShiftY - oldY) != 0);
	globalShiftMutex.unlock();

// Contentmatrix resizen
	if(shifted) {
		contentMutex.lock();
		int newX = max(content.size(), content.size() + (globalShiftX - oldX));
		int newY = max(content[0].size(), content[0].size() + (globalShiftY - oldY));
		write_text_to_log_file("Resize Content Matrix from: " + std::to_string(content.size()) + " - " + std::to_string(content[0].size()));
		write_text_to_log_file("Resize Content Matrix to: " + std::to_string(newX) + " - " + std::to_string(newY));
		content.resize(newX);
		for (int i = 0; i < content.size(); i++) {
			content[i].resize(newY, ' ');
		}
		contentMutex.unlock();
	}

	if(shifted) {
		renderScreen();
	}else{
		screenMutex.lock();
		refresh();
		screenMutex.unlock();
	}
}

// Setzt einen Char in der Content Matrix
void setChar(int x, int y, char c, bool send) {
	contentMutex.lock();
	if(x >= content.size() || y >= content[0].size()) {
		content.resize(x+1);
		for (int i = 0; i < content.size(); i++) {
			content[i].resize(y+1, ' ');
		}
	}
	write_text_to_log_file("Setze zeiche hier: " + std::to_string(x) + "-" + std::to_string(y+3) + "~" + std::to_string((char) c));
	content[x][y] = c;
	contentMutex.unlock();
// Update an alle Peers Senden
	if(send) {
		pthread_t sendToAll;
		string *msg = new string("MSG_X_" + to_string(x) + "_Y_" + to_string(y) + "_C_" + c);
		myClockMutex.lock();
		*msg = *msg + "_IP_"+local_ip+"_PORT_"+std::to_string(local_port)+"_C_"+std::to_string(myClock)+"_";
		myClockMutex.unlock();
		for(int i = 0; i < peers.size(); i++) {
			*msg = *msg + "_IP_"+getIpByFd(peers[i].peerFd)+"_PORT_"+std::to_string(getPortByFd(peers[i].peerFd))+"_C_"+std::to_string(peers[i].clock)+"_";
		}
		if(pthread_create(&sendToAll, NULL , sendThisToAll, msg)!=0) {
			cout<<"ERROR: Es konnte kein Thread erstellt werden.";
			close(rcvFd);
			exit(-1);
		}
	}
	renderSingleScreen(x, y);
//	renderScreen();
}

// Problemstellung:
// Es wird an den empfangssocket gesendet.
// Bei New muss sich der Socket identifiezen

// Umgang mit einem verbundenen Client (Threadfunktion)
void handleNewClientCon(int cfd) {
	string currentMeesage;
	ssize_t bytes;
	char str[128];

	while(1) {
		if((bytes = read(cfd, str, sizeof(str))) > 0) {
			write_text_to_log_file(string("Angekommen: ") + str);
			if(bytes < 0) {
				die("ERROR: Konnte keine Daten empfangen!");
			}

			currentMeesage.append(str);
		}

		int antwortPeer = -1;
		int newPeer = -2;

		while(currentMeesage.find(trenner) != std::string::npos) {
			string s = currentMeesage.substr(0, currentMeesage.find(trenner));
			currentMeesage = currentMeesage.substr(s.length()+3);
			string msgType = s.substr(0, 3);
			s = s.substr(3);
			string str2 = s;

			write_text_to_log_file("Verarbeitung: " + s);

	//		write_text_to_log_file(msgType);

			if(msgType.compare("NEW") == 0) {
				str2 = str2.substr(4);
				string ip = str2.substr(0, str2.find("_"));
				str2 = str2.substr(str2.find("_"));
				int port = stoi(str2.substr(6));

				write_text_to_log_file("New Peer empfangen: " + ip + " - " +std::to_string(port));

				antwortPeer = -1;

				write_text_to_log_file("Anzahl bekannter Peers: "+ std::to_string(peers.size()));
				for(int i = 0; i < peers.size(); i++) {
					write_text_to_log_file("FD: " + std::to_string(peers[i].peerFd));
					write_text_to_log_file("Adresse: " + peers[i].peerAdresse);
					write_text_to_log_file("Port: " + std::to_string(peers[i].peerPort));
					if((ip.compare(peers[i].peerAdresse) == 0) && (port == peers[i].peerPort)) {
						antwortPeer = peers[i].peerFd;
						write_text_to_log_file("Verbiundung schon bekannt: " + peers[i].peerPort);
				//		write_text_to_log_file("Peer ist schon bekannt: " + std::to_string(ip.compare(peers[i].peerAdresse)) + 
				//			" - " + std::to_string(peers[i].peerPort)));
					}
				}
				// Zu Peer verbinden, wenn noch nicht bekannt
				newPeer = -2;
				if(antwortPeer < 0) {
					struct sockaddr_in n_addr;

					// Zu einer aktiven Sitzung verbinden
					n_addr.sin_family = AF_INET;
					n_addr.sin_addr.s_addr = inet_addr(ip.c_str());
					n_addr.sin_port = htons(port);

//					write_text_to_log_file("Das ok?");		

					if((newPeer = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
						die("ERROR: Es konnte kein Socket geöffnet werden.");
					}

//					write_text_to_log_file("Das ok?");

					int ret = 0;
					if((ret = connect(newPeer, (struct sockaddr*) &n_addr, sizeof(n_addr))) < 0) {
						write_text_to_log_file("Errno: " + std::to_string(errno) + " - " + string(strerror(errno)));
						write_text_to_log_file("Open Socket: " + string(ip) + " - " + std::to_string(port));
						cout << "Return: " << strerror(errno) << endl;
						die("ERROR: Es konnte nicht zum Sockt verbunden werden.");
					}
					write_text_to_log_file("Neue Verbindung aufbauen: " + ip + " - " + std::to_string(port));

					//Peer hinzufügen
					antwortPeer = newPeer;
				}

				write_text_to_log_file("Antworten über FD: " + std::to_string(antwortPeer));

				peerMutex.lock();
				for(int i = 0; i < peers.size(); i++) {
					string peer_ip = peers[i].peerAdresse;
					int peer_port = peers[i].peerPort;
//					sendThis(cfd, "PER_IP_" + peer_ip + "_PORT_" + std::to_string(peer_port));
					write_text_to_log_file("Peer Informationen senden: " + std::to_string(peers[i].peerFd) + " - " + 
						peer_ip + " Port: " + std::to_string(peer_port));
					sendThis(antwortPeer, "PER_IP_" + local_ip + "_PORT_" + std::to_string(peer_port));
				}
				peerMutex.unlock();

				if(antwortPeer == newPeer) {
					peerMutex.lock();
					peers.resize(peers.size()+1);
					peers[peers.size()-1].peerFd = newPeer;
					peers[peers.size()-1].peerAdresse = ip;
					peers[peers.size()-1].peerPort = port;
					peers[peers.size()-1].clock = 0;
					write_text_to_log_file("Neuer Peer eingetragen: " + std::to_string(newPeer) + " - " + ip + " - " + std::to_string(port));
					peerMutex.unlock();

					refreshHeader(local_ip, local_port, peers.size());
				}

			// Aktuellen Stand des Pads übersenden
				contentMutex.lock();
				for(int i = 0; i < content.size(); i++) {
					for(int j = 0; j < content[i].size(); j++) {
						if(content[i][j]==' ') {
							continue;
						}
						sendThis(antwortPeer, "CRR_X_"+std::to_string(i)+"_Y_"+std::to_string(j)+"_C_"+content[i][j]);
					}
				}
				contentMutex.unlock();
				// Mitteilen, das alles übertragen wurde
				sendThis(antwortPeer, "RDY");
			}else if(msgType.compare("PER") == 0) {
				str2 = str2.substr(4);
				string ip = str2.substr(0, str2.find("_"));
				str2 = str2.substr(str2.find("_"));
				int port = stoi(str2.substr(6));

				write_text_to_log_file("Peer empfangen: " + ip + " - " + std::to_string(port));

				for(int i = 0; i < peers.size(); i++) {
					if((ip.compare(peers[i].peerAdresse) == 0) && (port == peers[i].peerPort)) {
						antwortPeer = peers[i].peerFd;
						write_text_to_log_file("Verbiundung schon bekannt: " + std::to_string(peers[i].peerPort));
					}
				}
				// Zu Peer verbinden, wenn noch nicht bekannt
				if(antwortPeer < 0) {
					struct sockaddr_in n_addr;

					// Zu einer aktiven Sitzung verbinden
					n_addr.sin_family = AF_INET;
					n_addr.sin_addr.s_addr = inet_addr(ip.c_str());
					n_addr.sin_port = htons(port);

//					write_text_to_log_file("Das ok?");

					write_text_to_log_file("Verbinden zu: " + ip + " - " + std::to_string(port));		

					if((newPeer = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
						die("ERROR: Es konnte kein Socket geöffnet werden.");
					}

//					write_text_to_log_file("Das ok?");

					int ret = 0;
					if((ret = connect(newPeer, (struct sockaddr*) &n_addr, sizeof(n_addr))) < 0) {
						write_text_to_log_file("Errno: " + std::to_string(errno) + " - " + string(strerror(errno)));
						write_text_to_log_file("Open Socket: " + string(ip) + " - " + std::to_string(port));
						die("ERROR: Es konnte nicht zum Sockt verbunden werden.");
					}

					//Peer hinzufügen
					peers.resize(peers.size()+1);
					peers[peers.size()-1].peerFd = newPeer;
					peers[peers.size()-1].peerAdresse = ip;
					peers[peers.size()-1].peerPort = port;
					peers[peers.size()-1].clock = 0;
					write_text_to_log_file("Neuer Peer eingetragen: " + std::to_string(newPeer) + " - " + ip + " - " + std::to_string(port));

					sendThis(newPeer, "PER_IP_" + local_ip + "_PORT_" + std::to_string(local_port));
					write_text_to_log_file("NEW Peer added to Peer List: " + std::to_string(newPeer));
					refreshHeader(local_ip, local_port, peers.size());
				}
			}else if(msgType.compare("MSG") == 0) {
				write_text_to_log_file("MSG wird verarbeitet: " + s);
				// Empfange eine Nachricht
				str2 = str2.substr(3);
				int x = stoi(str2.substr(0, str2.find("_")));
				str2 = str2.substr(str2.find("_"));

				str2 = str2.substr(3);
				int y = stoi(str2.substr(0, str2.find("_")));
				str2 = str2.substr(str2.find("_"));
				str2 = str2.substr(3);
				char c = str2.substr(0, 1).at(0);
				str2 = str2.substr(1);

				string ipOfPort = getIpByFd(cfd);
				int portOfPeer = getPortByFd(cfd);

				while(str2.length() > 0) {
					// "_IP_" entfernen
					str2 = str2.substr(4);
					string ip = str2.substr(0, str2.find("_"));
//					write_text_to_log_file("IP: " + ip);
//					write_text_to_log_file("String: " + str2);
					str2 = str2.substr(str2.find("_"));
					// "_PORT_" entfernen
					str2 = str2.substr(6);
					int port = stoi(str2.substr(0, str2.find("_")));
//					write_text_to_log_file("Port: " + std::to_string(port));
//					write_text_to_log_file("String: " + str2);
					str2 = str2.substr(str2.find("_"));
					// "_C_" entfernen
					str2 = str2.substr(3);
					int clock = stoi(str2.substr(0, str2.find("_")));
//					write_text_to_log_file("Clock: " + std::to_string(clock));
//					write_text_to_log_file("String: " + str2);
					str2 = str2.substr(str2.find("_"));
					// "_" entfernen
					str2 = str2.substr(1);
//					write_text_to_log_file("String am ende: " + str2);

					peerMutex.lock();
					for(int i = 0; i < peers.size(); i++) {
//						write_text_to_log_file("IP: " + ip);
//						write_text_to_log_file("Port: " + std::to_string(port));
						if((peers[i].peerAdresse.compare(ip) == 0) && (peers[i].peerPort == port)) {
					//		if(peers[i].clock <= clock) {
								peerMutex.unlock();
								setChar(x, y, c, false);
								peerMutex.lock();
					//		}
						}
						peers[i].clock = max(peers[i].clock, clock);
					}
					peerMutex.unlock();
				}
				
			}else if(msgType.compare("CRR") == 0) {
				write_text_to_log_file("CRR empfangen");
				// Empfange eine Nachricht
				str2 = str2.substr(3);
				int x = stoi(str2.substr(0, str2.find("_")));
				str2 = str2.substr(str2.find("_"));

				str2 = str2.substr(3);
				int y = stoi(str2.substr(0, str2.find("_")));
				str2 = str2.substr(str2.find("_"));
				str2 = str2.substr(3);
				char c = str2.substr(0, 1).at(0);

				setChar(x, y, c, false);
			}else if(msgType.compare("RDY") == 0) {
				write_text_to_log_file("RDY Recieved");
				newLock.unlock();
			}else {
				write_text_to_log_file("Unbekannter Nachrichtentyp: " + msgType);
			}
			for(int i=0;i<sizeof(str);i++) {
				str[i] = '\0';
			}
		}
	}
	exit(1);
}

// Starten des empfangsthreads
void setup_rcv_thread() {
	rcv_addr.sin_family = AF_INET;
	rcv_addr.sin_port = 0;
	rcv_addr.sin_addr.s_addr = INADDR_ANY;

	if((rcvFd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		die("ERROR: Konnte Socket nicht erstellen.");
	}

	if(bind(rcvFd, (struct sockaddr*) &rcv_addr, sock_t) < 0) {
		die("ERROR: Konnte Socket nicht binden.");
	}

	if(listen(rcvFd, 1) < 0) {
		die("ERROR: Konnte Socket nicht hören.");
	}

	getsockname(rcvFd, (struct sockaddr*) &rcv_addr, &sock_t);

	local_port = ntohs(rcv_addr.sin_port);

	rdLock.unlock();

	while(1) {
		int cfd;
		cfd = accept(rcvFd, (struct  sockaddr*) &newCon_addr, &sock_t);
		write_text_to_log_file("Neue Verbindung akzeptiert mit FD: " + std::to_string(cfd));
		if(cfd < 0) {
			continue;
		}

		pthread_t newThread;

		if(pthread_create(&newThread, NULL , handleNewClientCon, cfd)!=0) {
			write_text_to_log_file("ERROR: Es konnte kein Thread erstellt werden.");
			close(rcvFd);
			exit(-1);
		}
	}
	exit(1);
}

// Mainfunktion
int main(int argc, char const *argv[]) {
	debug = true;
	myClockMutex.lock();
	myClock = 0;
	local_ip = "127.0.0.1";
	myClockMutex.unlock();
	trenner = "<->";
	local_port = 0;

	struct ifaddrs *ifap, *ifa;
    struct sockaddr_in *sa;
    char *addr;

/*    getifaddrs (&ifap);
    for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr->sa_family==AF_INET) {
            sa = (struct sockaddr_in *) ifa->ifa_addr;
            addr = inet_ntoa(sa->sin_addr);

        //    printf("Interface: %s\tAddress: %s\n", ifa->ifa_name, addr);

            if(((string(ifa->ifa_name)).compare("lo")) != 0) {
          //  	printf("Interface: %s\tAddress: %s\n", ifa->ifa_name, addr);
            	local_ip = addr;
            }
        }
    }

    freeifaddrs(ifap);*/

//	local_ip = inet_ntoa(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr);

	write_text_to_log_file("----------------------------------------------");
	rdLock.lock();

	initscr();
	atexit(quit);
	noecho();
	cbreak();
	keypad(stdscr, TRUE);
//	nodelay(stdscr, TRUE);
	start_color();
	curs_set(0);

	peerMutex.lock();
	peers.resize(0);
	peerMutex.unlock();

	int w;
	int h;
	getmaxyx(stdscr, h, w);

	globalShiftMutex.lock();
	globalShiftX = 0;
	globalShiftY = 0;
	globalShiftMutex.unlock();

	contentMutex.lock();
	content.resize(w);
	for (int i = 0; i < w; i++) {
    	content[i].resize(h-4);
	}
	for(int i = 0; i < content.size(); i++) {
		for(int j = 0; j < content[i].size(); j++) {
			content[i][j] = ' ';
		}
	}
	contentMutex.unlock();

	// Prozess Teilen
	if(pthread_create(&rcvThread, NULL , setup_rcv_thread, NULL)!=0) {
		write_text_to_log_file("ERROR: Es konnte kein Thread erstellt werden.");
		close(rcvFd);
		exit(-1);
	}

	rdLock.lock();
	rdLock.unlock();
	write_text_to_log_file("unlocked");

	if(argc == 3) {
		int sndFd;
		// Zu einer aktiven Sitzung verbinden
		host_addr.sin_family = AF_INET;
		host_addr.sin_addr.s_addr = inet_addr(argv[1]);
		host_addr.sin_port = htons(atoi(argv[2]));

		if((sndFd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
			write_text_to_log_file("Errno: " + std::to_string(errno) + " - " + string(strerror(errno)));
			write_text_to_log_file("Open Socket: " + string(argv[1]) + " - " + std::to_string(atoi(argv[2])));
			die("ERROR: Es konnte kein Socket geöffnet werden.");
		}

		int ret = 0;
		if((ret = connect(sndFd, (struct sockaddr*) &host_addr, sizeof(host_addr))) < 0) {
			write_text_to_log_file("Errno: " + std::to_string(errno) + " - " + string(strerror(errno)));
			write_text_to_log_file("Open Socket: " + string(argv[1]) + " - " + std::to_string(atoi(argv[2])) + " - " + std::to_string(ret));
			cout << "Return: " << strerror(errno) << endl;
			die("ERROR: Es konnte nicht zum Sockt verbunden werden.");
		}

//		sleep(1);

		//Mentor Peer als Peer aufnehmen
		peers.resize(peers.size()+1);
		peers[peers.size()-1].peerFd = sndFd;
		peers[peers.size()-1].peerAdresse = argv[1];
		peers[peers.size()-1].peerPort = atoi(argv[2]);
		write_text_to_log_file("Neuer Peer eingetragen: " + std::to_string(sndFd) + " - " + argv[1] + " - " + std::to_string(atoi(argv[2])));
		refreshHeader(local_ip, local_port, peers.size());

		sendThis(sndFd, "NEW_IP_"+local_ip+"_PORT_"+std::to_string(local_port));

//Warten, bis Daten empfangen wurden
		newLock.lock();
		newLock.unlock();

	}else if(argc != 1) {
		cout<<"Falsche Parameterzahl. Nutze:\n";
		cout<<"./client - Um eine neue Sitzung zu starten.\n";
		cout<<"./client <hostIP> <hostPort> - Um einer Sitzung beizutreten.\n";
		putc('\n', stderr);
		exit(-1);
	}

// Create Color Sets
	init_pair(0, 7, 0);		// Default, Weiß auf schwarz
	init_pair(1, 0, 6);		// Trennlinie, Schwarz auf Türkis
	init_pair(2, 2, 0);		// OK, Grün auf Schwarz

	clearScreen();
	refreshHeader(local_ip, local_port, peers.size());
	refreshFooter();

	screenMutex.lock();
	refresh();
	screenMutex.unlock();

	int ch;
	int cPosX = 0;
	int cPosY = 0;
	move(3, 0);
	curs_set(true);

// Programm Main Loop
	while(1) {
		getyx(stdscr, cPosY, cPosX);
		if ((ch = getch()) != ERR) {
			if(ch == KEY_F(6) && saveFlag) {
				saveFlag = false;
				save();
			}else{
				saveFlag = true;
				if (ch == KEY_F(10)) {
					exit(0);
				}else if(ch == KEY_F(6)) {
					// Ignore F6, wenn nicht gespeichert werden kann
				}else if (ch == 10) {
					// Enter Char abfangen
					updateCursor(-cPosX, 1);
				}else if (ch == KEY_UP) {
					updateCursor(0, -1);
				}else if (ch == KEY_DOWN) {
					updateCursor(0, 1);
				}else if (ch == KEY_LEFT) {
					updateCursor(-1, 0);
				}else if (ch == KEY_RIGHT) {
					updateCursor(1, 0);
				}else if(ch == KEY_BACKSPACE) {
					updateCursor(-1, 0);
					globalShiftMutex.lock();
					int gX = globalShiftX;
					int gY = globalShiftY;
					globalShiftMutex.unlock();
					getyx(stdscr, cPosY, cPosX);
	//				mvaddch(cPosY, cPosX, ' ');
					setChar(cPosX + gX, cPosY + gY - 3, ' ', true);
					move(cPosY, cPosX);
					
				}else if(ch == 258 || ch == 259) {
					//Scrollen verwerfen
				}else{
					globalShiftMutex.lock();
					int gX = globalShiftX;
					int gY = globalShiftY;
					globalShiftMutex.unlock();
					setChar(cPosX + gX, cPosY + gY - 3, ch, true);
					updateCursor(1, 0);
	        	}
	        }
		}
	}
	return 0;
}
















