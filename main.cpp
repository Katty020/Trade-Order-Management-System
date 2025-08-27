#include <bits/stdc++.h>
using namespace std;

struct Login {
    string userName;
    string passWord;
};

struct Logout {
    string userName;
};

struct TradeOrder {
    int symbolId;
    double price;
    uint64_t quantity;
    char side;
    uint64_t orderId;
};

enum class RequestKind { Unknown = 0, New = 1, Modify = 2, Cancel = 3 };
enum class ResponseKind { Unknown = 0, Accepted = 1, Rejected = 2 };

struct TradeResponse {
    uint64_t orderId;
    ResponseKind responseKind;
};

class TradeOrderManager {
private:
    queue<TradeOrder> pendingQueue;
    unordered_map<uint64_t, TradeOrder*> orderLookup;
    unordered_map<uint64_t, chrono::system_clock::time_point> waitingOrders;
    
    mutex queueLock, waitingLock, fileLock;
    condition_variable orderCondition;
    atomic<bool> runningFlag;
    
    thread orderThread, sessionThread, responseThread;

    int maxOrdersPerSecond;
    int sentOrdersCount;
    chrono::system_clock::time_point secondStart;

    string sessionStartTime;
    string sessionEndTime;
    bool tradingActive;
    ofstream responseFile;

    RequestKind findRequestKind(const TradeOrder& order) {
        return RequestKind::New;
    }

    string getTimeString() {
        auto now = chrono::system_clock::now();
        time_t nowTime = chrono::system_clock::to_time_t(now);
        tm* localTime = localtime(&nowTime);
        
        stringstream ss;
        ss << setw(2) << setfill('0') << localTime->tm_hour << ":"
           << setw(2) << setfill('0') << localTime->tm_min << ":"
           << setw(2) << setfill('0') << localTime->tm_sec;
        return ss.str();
    }

    void fakeExchangeResponses() {
        while (runningFlag) {
            this_thread::sleep_for(chrono::milliseconds(100));
            
            vector<uint64_t> readyOrders;
            {
                lock_guard<mutex> lock(waitingLock);
                for (const auto& entry : waitingOrders) {
                    readyOrders.push_back(entry.first);
                }
            }
            
            for (uint64_t oid : readyOrders) {
                TradeResponse reply;
                reply.orderId = oid;
                reply.responseKind = (rand() % 10 > 1) ? ResponseKind::Accepted : ResponseKind::Rejected;
                
                onData(move(reply));
            }
        }
    }

public:
    TradeOrderManager(int rateLimit, const string& start, const string& end)
        : maxOrdersPerSecond(rateLimit), sentOrdersCount(0),
          sessionStartTime(start), sessionEndTime(end),
          tradingActive(false), runningFlag(true) {
        
        secondStart = chrono::system_clock::now();
        responseFile.open("order_responses.log", ios::app);
        
        if (!responseFile.is_open()) {
            cerr << "ERROR: Could not open response log file!" << endl;
        } else {
            responseFile << "=== Log Started at " << getTimeString() << " ===" << endl;
        }
        
        orderThread = thread(&TradeOrderManager::processOrders, this);
        sessionThread = thread(&TradeOrderManager::watchSession, this);
        responseThread = thread(&TradeOrderManager::fakeExchangeResponses, this);
        
        cout << "Order Manager ready. Session: " << start << " to " << end << endl;
    }

    ~TradeOrderManager() {
        runningFlag = false;
        orderCondition.notify_all();
        
        if (orderThread.joinable()) orderThread.join();
        if (sessionThread.joinable()) sessionThread.join();
        if (responseThread.joinable()) responseThread.join();
        
        if (responseFile.is_open()) {
            responseFile << "=== Log Ended at " << getTimeString() << " ===" << endl;
            responseFile.close();
        }
        
        cout << "Order Manager shut down." << endl;
    }

    void onData(TradeOrder&& order) {
        lock_guard<mutex> lock(queueLock);
        
        if (!tradingActive) {
            cout << "REJECTED - Order " << order.orderId << " outside session hours" << endl;
            return;
        }

        RequestKind kind = findRequestKind(order);
        
        if (kind == RequestKind::Modify || kind == RequestKind::Cancel) {
            auto it = orderLookup.find(order.orderId);
            if (it != orderLookup.end()) {
                if (kind == RequestKind::Modify) {
                    it->second->price = order.price;
                    it->second->quantity = order.quantity;
                    cout << "MODIFIED - Order " << order.orderId 
                         << " updated. New price: " << order.price 
                         << ", qty: " << order.quantity << endl;
                    return;
                } else if (kind == RequestKind::Cancel) {
                    orderLookup.erase(it);
                    cout << "CANCELLED - Order " << order.orderId << " removed" << endl;
                    return;
                }
            }
        }

        if (kind == RequestKind::New) {
            pendingQueue.push(order);
            orderLookup[order.orderId] = &pendingQueue.back();
            orderCondition.notify_one();
            cout << "QUEUED - Order " << order.orderId << " added" << endl;
        }
    }

    void onData(TradeResponse&& reply) {
        lock_guard<mutex> lock(waitingLock);
        auto it = waitingOrders.find(reply.orderId);
        if (it != waitingOrders.end()) {
            auto now = chrono::system_clock::now();
            auto latency = chrono::duration_cast<chrono::microseconds>(now - it->second).count();
            
            logReply(reply, latency);
            waitingOrders.erase(it);
        }
    }

    void sendOrder(const TradeOrder& order) {
        cout << "SENT - Order " << order.orderId 
             << " [Symbol: " << order.symbolId 
             << ", Price: " << order.price 
             << ", Qty: " << order.quantity 
             << ", Side: " << order.side << "]" << endl;
    }

    void sendLogin() {
        cout << "LOGIN SENT" << endl;
    }

    void sendLogout() {
        cout << "LOGOUT SENT" << endl;
    }

private:
    void processOrders() {
        while (runningFlag) {
            unique_lock<mutex> lock(queueLock);
            orderCondition.wait(lock, [this] { 
                return !pendingQueue.empty() || !runningFlag; 
            });
            
            if (!runningFlag) break;
            
            updateSecondCounter();
            
            while (!pendingQueue.empty() && sentOrdersCount < maxOrdersPerSecond && tradingActive) {
                TradeOrder order = pendingQueue.front();
                pendingQueue.pop();
                orderLookup.erase(order.orderId);
                
                lock.unlock();
                
                {
                    lock_guard<mutex> waiting(waitingLock);
                    waitingOrders[order.orderId] = chrono::system_clock::now();
                }
                
                sendOrder(order);
                sentOrdersCount++;
                
                lock.lock();
            }
            
            this_thread::sleep_for(chrono::milliseconds(1));
        }
    }

    void watchSession() {
        while (runningFlag) {
            string nowTime = getTimeString().substr(0, 5);
            
            bool active = (nowTime >= sessionStartTime && nowTime < sessionEndTime);
            
            if (active && !tradingActive) {
                tradingActive = true;
                sendLogin();
                cout << "SESSION STARTED at " << nowTime << endl;
            } else if (!active && tradingActive) {
                tradingActive = false;
                sendLogout();
                cout << "SESSION ENDED at " << nowTime << endl;
                
                lock_guard<mutex> lock(queueLock);
                while (!pendingQueue.empty()) pendingQueue.pop();
                orderLookup.clear();
            }
            
            this_thread::sleep_for(chrono::seconds(1));
        }
    }

    void updateSecondCounter() {
        auto now = chrono::system_clock::now();
        if (now - secondStart >= chrono::seconds(1)) {
            secondStart = now;
            sentOrdersCount = 0;
        }
    }

    void logReply(const TradeResponse& reply, long latency) {
        lock_guard<mutex> lock(fileLock);
        if (responseFile.is_open()) {
            responseFile << "Time: " << getTimeString()
                        << ", OrderID: " << reply.orderId
                        << ", Reply: " << (reply.responseKind == ResponseKind::Accepted ? "Accepted" : "Rejected")
                        << ", Latency: " << latency << "μs" << endl;
            responseFile.flush();
        }
        cout << "REPLY - Order " << reply.orderId 
             << " [" << (reply.responseKind == ResponseKind::Accepted ? "Accepted" : "Rejected")
             << ", Latency: " << latency << "μs]" << endl;
    }
};

// Test function
void runTest() {
    TradeOrderManager manager(2, "00:00", "23:59");
    
    TradeOrder order1{1, 100.50, 100, 'B', 1001};
    TradeOrder order2{2, 200.75, 50, 'S', 1002};
    TradeOrder order3{3, 150.25, 75, 'B', 1003};
    TradeOrder order4{4, 300.00, 25, 'S', 1004};
    TradeOrder orderModify{1, 105.00, 80, 'B', 1001};
    TradeOrder orderCancel{2, 0, 0, 'S', 1002};
    
    cout << "\n=== Running Trade Order Manager Test ===\n" << endl;
    
    this_thread::sleep_for(chrono::seconds(1));
    
    manager.onData(move(order1));
    manager.onData(move(order2));
    manager.onData(move(order3));
    manager.onData(move(order4));
    
    this_thread::sleep_for(chrono::seconds(2));
    
    manager.onData(move(orderModify));
    manager.onData(move(orderCancel));
    
    this_thread::sleep_for(chrono::seconds(5));
}

int main() {
    srand(time(0));
    runTest();
    return 0;
}
