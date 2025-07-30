#include <iostream>
#include <fstream>
#include <string>
#include <map>
#include <vector>
#include <thread>
#include <chrono>
#include <syslog.h>
#include <unistd.h>
#include <cstdlib>
#include <signal.h>
#include <cstring>
#include <PCSC/winscard.h>

class SmartCardLock {
private:
    SCARDCONTEXT hContext;
    std::map<std::string, std::vector<std::string>> cardUserMap;
    std::string configPath;
    std::map<std::string, bool> userCardPresent;
    bool running;

public:
    SmartCardLock(const std::string& configFile = "/etc/smartcard_login.conf") 
        : configPath(configFile), running(true) {
        openlog("smartcard_lock", LOG_PID, LOG_AUTH);
        
        LONG rv = SCardEstablishContext(SCARD_SCOPE_SYSTEM, NULL, NULL, &hContext);
        if (rv != SCARD_S_SUCCESS) {
            syslog(LOG_ERR, "Failed to establish PC/SC context: %08lX", rv);
            throw std::runtime_error("Failed to establish PC/SC context");
        }
        
        loadConfiguration();
    }

    ~SmartCardLock() {
        running = false;
        SCardReleaseContext(hContext);
        closelog();
    }

    void loadConfiguration() {
        cardUserMap.clear();
        userCardPresent.clear();
        
        std::ifstream configFile(configPath);
        std::string line;
        
        while (std::getline(configFile, line)) {
            if (line.empty() || line[0] == '#') continue;
            
            size_t colonPos = line.find(':');
            if (colonPos != std::string::npos) {
                std::string cardId = line.substr(0, colonPos);
                std::string username = line.substr(colonPos + 1);
                cardUserMap[cardId].push_back(username);
                userCardPresent[username] = false;
                syslog(LOG_INFO, "Loaded mapping: card %s -> user %s", 
                       cardId.c_str(), username.c_str());
            }
        }
    }

    std::vector<std::string> getReaders() {
        std::vector<std::string> readers;
        DWORD dwReaders = SCARD_AUTOALLOCATE;
        LPSTR mszReaders = NULL;
        
        LONG rv = SCardListReaders(hContext, NULL, (LPSTR)&mszReaders, &dwReaders);
        if (rv == SCARD_S_SUCCESS) {
            char* reader = mszReaders;
            while (*reader) {
                readers.push_back(std::string(reader));
                reader += strlen(reader) + 1;
            }
            SCardFreeMemory(hContext, mszReaders);
        }
        
        return readers;
    }

    std::string getCardATR(const std::string& readerName) {
        SCARDHANDLE hCard;
        DWORD dwActiveProtocol;
        
        LONG rv = SCardConnect(hContext, readerName.c_str(), 
                              SCARD_SHARE_SHARED, SCARD_PROTOCOL_T0 | SCARD_PROTOCOL_T1,
                              &hCard, &dwActiveProtocol);
        
        if (rv != SCARD_S_SUCCESS) {
            return "";
        }
        
        BYTE pbAtr[MAX_ATR_SIZE];
        DWORD dwAtrLen = sizeof(pbAtr);
        DWORD dwReaderLen = 0, dwState = 0, dwProtocol = 0;
        
        rv = SCardStatus(hCard, NULL, &dwReaderLen, &dwState, &dwProtocol, pbAtr, &dwAtrLen);
        
        SCardDisconnect(hCard, SCARD_LEAVE_CARD);
        
        if (rv != SCARD_S_SUCCESS) {
            return "";
        }
        
        std::string atr;
        for (DWORD i = 0; i < dwAtrLen; i++) {
            char hex[3];
            sprintf(hex, "%02X", pbAtr[i]);
            atr += hex;
        }
        
        return atr;
    }

    bool isUserLoggedIn(const std::string& username) {
        std::string checkCmd = "loginctl list-sessions --no-legend | grep -w " + username + " | grep active";
        FILE* pipe = popen(checkCmd.c_str(), "r");
        bool loggedIn = false;
        
        if (pipe) {
            char buffer[256];
            if (fgets(buffer, sizeof(buffer), pipe)) {
                loggedIn = true;
            }
            pclose(pipe);
        }
        
        return loggedIn;
    }

    void lockUserSession(const std::string& username) {
        if (!isUserLoggedIn(username)) {
            return;
        }
        
        // Get user's session ID
        std::string sessionCmd = "loginctl list-sessions --no-legend | grep -w " + username + " | awk '{print $1}' | head -1";
        FILE* pipe = popen(sessionCmd.c_str(), "r");
        
        if (pipe) {
            char sessionId[32];
            if (fgets(sessionId, sizeof(sessionId), pipe)) {
                sessionId[strcspn(sessionId, "\n")] = 0;
                
                // Lock the session
                std::string lockCmd = "loginctl lock-session " + std::string(sessionId);
                if (system(lockCmd.c_str()) == 0) {
                    syslog(LOG_INFO, "Locked session %s for user %s due to smart card removal", 
                           sessionId, username.c_str());
                } else {
                    syslog(LOG_ERR, "Failed to lock session %s for user %s", 
                           sessionId, username.c_str());
                }
            }
            pclose(pipe);
        }
    }

    std::vector<std::string> getActiveUsers() {
        std::vector<std::string> activeUsers;
        FILE* pipe = popen("loginctl list-sessions --no-legend | awk '{print $3}' | grep -v root | sort -u", "r");
        
        if (pipe) {
            char username[256];
            while (fgets(username, sizeof(username), pipe)) {
                username[strcspn(username, "\n")] = 0; // Remove newline
                if (strlen(username) > 0) {
                    activeUsers.push_back(std::string(username));
                }
            }
            pclose(pipe);
        }
        
        return activeUsers;
    }

    void handleCardRemoval(const std::string& cardId) {
        // Check if this card ATR matches any user from config
        auto it = cardUserMap.find(cardId);
        if (it != cardUserMap.end()) {
            // Get all currently active users
            std::vector<std::string> activeUsers = getActiveUsers();
            
            // Find the first active user who can use this card and has it present
            for (const std::string& configUser : it->second) {
                // Check if this configured user is currently active
                for (const std::string& activeUser : activeUsers) {
                    if (configUser == activeUser && userCardPresent[activeUser]) {
                        userCardPresent[activeUser] = false;
                        syslog(LOG_INFO, "Smart card removed for user %s, locking screen", activeUser.c_str());
                        lockUserSession(activeUser);
                        
                        // Mark card as not present for ALL users to prevent duplicate processing
                        for (const std::string& allConfigUser : it->second) {
                            userCardPresent[allConfigUser] = false;
                        }
                        return; // Exit after locking the first active user
                    }
                }
            }
        }
    }

    void handleCardInsertion(const std::string& cardId) {
        // Mark card as present for all users who can use this card
        auto it = cardUserMap.find(cardId);
        if (it != cardUserMap.end()) {
            for (const std::string& username : it->second) {
                userCardPresent[username] = true;
                syslog(LOG_INFO, "Smart card inserted for user %s", username.c_str());
            }
        }
    }

    void monitorCards() {
        syslog(LOG_INFO, "Starting smart card lock monitoring service");
        
        // Initialize card states
        std::map<std::string, std::string> lastCardStates;
        
        while (running) {
            std::vector<std::string> readers = getReaders();
            std::map<std::string, std::string> currentCardStates;
            
            for (const auto& reader : readers) {
                std::string cardId = getCardATR(reader);
                if (!cardId.empty()) {
                    currentCardStates[reader] = cardId;
                }
            }
            
            // Check for card insertions
            for (const auto& [reader, cardId] : currentCardStates) {
                if (lastCardStates.find(reader) == lastCardStates.end() || 
                    lastCardStates[reader] != cardId) {
                    handleCardInsertion(cardId);
                }
            }
            
            // Check for card removals
            for (const auto& [reader, cardId] : lastCardStates) {
                if (currentCardStates.find(reader) == currentCardStates.end()) {
                    handleCardRemoval(cardId);
                }
            }
            
            lastCardStates = currentCardStates;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    void stop() {
        running = false;
    }
};

SmartCardLock* lockService = nullptr;

void signalHandler(int) {
    if (lockService) {
        lockService->stop();
    }
}

int main(int argc, char* argv[]) {
    signal(SIGTERM, signalHandler);
    signal(SIGINT, signalHandler);
    
    try {
        std::string configPath = "/etc/smartcard_login.conf";
        if (argc > 1) {
            configPath = argv[1];
        }
        
        lockService = new SmartCardLock(configPath);
        lockService->monitorCards();
        
        delete lockService;
        lockService = nullptr;
        
    } catch (const std::exception& e) {
        syslog(LOG_ERR, "Fatal error: %s", e.what());
        return 1;
    }
    
    return 0;
}