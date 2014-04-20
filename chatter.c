/*
 * This system demonstrates the use of the socket() message passing to 
 * implement a distributed chat application.
 *
 * The chat app consists of 1 to 100 instances of the program "chatter"
 * which sets up a TCP port and both connects to any existing chatters in the
 * port range 1100-1200 (chosen more or less arbitrarily but causes no conflicts with
 * commonly used ports) and accepts connections from future chatters that join the 
 * system. 
 *
 * To use simply compile the given makefile and then open a new terminal
 * window to run each chatter executable in. Type in any terminal and all other 
 * running chatters in the correct port range will receive and output each line.
 * Type "!q" and press enter to quit any individual chatter; any remaining chatters
 * will continue to work no matter which one you quit.
 * 
 * Editing students: Zachary Levonian and Nathan Roberts
 * Code skeleton author: Sherri Goings
 * Last Modified: 3/6/2014
*/

#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <sys/poll.h>
#include <time.h>
#include <pthread.h>

//We like booleans
#define TRUE 1
#define FALSE 0

// messages may be at most 1024 characters long
size_t BUFFER_SIZE = 1024;

// socket address stores important info about specific type of socket connection
struct sockaddr_in address;
int addressSize;

//The port number this server or client is using; used as an id number for constructing token passing circle.
int portNum;

int mustStartTokenPassing = FALSE;

//the index of the socket/port that we forward the socket to
int tokenPassingBuddyIndex = -1;

//The token number of the last message received by this client or server
//The next expected message will be marked tokenNum+1
int tokenNum = 1;
int initializedTokenNum = FALSE;

//used to determine whether the messages are in the correct order
int waitingForExpectedMessages = FALSE;
int lastExpectedMessageValue = 1;

//used when a chatter disconnects
int mustSendConfirmation = FALSE;
int confirmationSocket = -1;

//the state of the token
int haveToken = FALSE;


// connected socket array, holds all nConnected sockets for this process, max 100.
int cs[100];
// parallel to the connected socket array, cp stores the port numbers.
int cp[100];
int nConnected = 0;

// num message received from each of the 100 possible connected sockets
int numReceived[100];

// array to hold threads for receiving messages on a given connection, need 1 
// thread per connection so at most 100 total.
pthread_t receivers[100];

// max amount of time in seconds a message may take to travel from one socket to another
int maxDelay = 2;

// sendBuffer holds up to 10 messages from this chatter that are waiting to be sent.
// each message has the string to be sent and the number of remaining connections to
// send that message to, so that when the last one is sent that message can be removed
// from the buffer.
// the buffer is implemented as a circular FIFO queue so startIndex is the position of 
// the current first message and nHeld is the total number of messages in the buffer
typedef struct sendMessage {
    char message[1024];
    int nToSend;
} sendMessage;
typedef struct toSendBuffer {
    sendMessage messages[10];
    int startIndex;
    int nHeld;
} toSendBuffer;


//A message that we have recieved, for use in a linked list
typedef struct receivedMessage {
    char message[1024];
    int tokenNum;
    int clientNum;
    struct receivedMessage* next;
} receivedMessage;

//the buffer
toSendBuffer sendBuffer;
receivedMessage* receivedBufferHead;

// lock to protect the sendBuffer as multiple threads access it
pthread_mutex_t sendBufLock;
//lock to protect the receivedBuffer
pthread_mutex_t receivedBufLock;
//Lock and condition to wait on while other threads confirm that we can disconnect
pthread_mutex_t disconnectLock;
pthread_cond_t disconnectCondition;
//Lock to protect the state of the token (here or not)
pthread_mutex_t tokenLock;


int joinNetwork(int port);
int createNetwork(int port);
int acceptConnection(int sock);
void* listenSocket(void*);
int getAndSend();
void* delaySend(void*);
void* delaySendToken(void*);
void* delaySendPortNum(void*);
void* delaySendDisconnect(void*);
void* delaySendDisconnectConfirm(void*);
void* acceptIncoming(void*);
int connectCurrent();
void* sendBufferedMessages(void*);
void processReceivedMessage(int, int, char*);
void printBuffer();
void receivedBuffer_add(int, char*, int);
struct receivedMessage receivedBuffer_pop();
void updateTokenChain(int);
int updateBrokenTokenChain(int);

int main(int argc, char* argv[])
{
    // user gives port number on which this process should accept incoming connections
    if (argc < 2 || argc > 3) {
        printf("usage: server <port number> [max send time (in seconds)]\n");
        return -1;
    }
    if (argc == 3) {
        maxDelay = atoi(argv[2]);
    }

    // initialize global vars where needed
    pthread_mutex_init(&sendBufLock, NULL);
    pthread_mutex_init(&tokenLock, NULL);
    pthread_mutex_init(&receivedBufLock, NULL);
    pthread_mutex_init(&disconnectLock, NULL);
    pthread_cond_init (&disconnectCondition, NULL);
    sendBuffer.startIndex = 0;
    sendBuffer.nHeld = 0;
    receivedBufferHead = NULL;
    int i;
    for (i = 0; i < 100; i++) {
        numReceived[i] = 0; //Initialize numreceived from all possible connections to 0
    }
    portNum = atoi(argv[1]);

    // seed the random number generator with the current time
    srand(time(NULL));

    // first connect to any existing chatters
    if (connectCurrent() == -1) return -1;

    if (nConnected == 0) {
        mustStartTokenPassing = TRUE;
    }

    // spin off threads to listen to already connected chatters and display their messages
    for (i=0; i<nConnected; i++) {
        pthread_create(&receivers[i], NULL, listenSocket, (void*)(long)i);
    }

    // set up this process's incoming TCP port for future connecting chatters
    int mys;
    mys = createNetwork(portNum);
    if (mys == -1) return -1;
    
    printf("Connecting to other chatters...\n");
    for (i=0; i<nConnected; i++) {
        long indexL = (long) i;
        void* args = (void*)((indexL << 32) + cs[i]);
        pthread_t send_port_thread;
        pthread_create(&send_port_thread, NULL, delaySendPortNum, args);
    }
    
    // spin off thread to listen and connect to future joining chatters
    pthread_t acceptor;
    pthread_create(&acceptor, NULL, acceptIncoming, (void*)(long)mys);

    // use this main thread to get user input and send it to all other chatters
    int quit = 0;
    while (!quit) {
        quit = getAndSend();
    }
    
    //Send notification of user disconnect
    int numDisconnectsToSend = nConnected;
    for (i=0; i<nConnected; i++) {
        long indexL = (long) i;
        void* args = (void*)((indexL << 32) + cs[i]);
        pthread_t send_disconnect_thread;
        pthread_create(&send_disconnect_thread, NULL, delaySendDisconnect, args);
    }
    
    //This code is designed to allow users to leave mid-chat cleanly.
    //However, in this current implementation, the final user hangs when they
    //attempt to exit.
    printf("waiting to disconnect...\n");
    pthread_mutex_lock(&disconnectLock);
    pthread_cond_wait(&disconnectCondition, &disconnectLock);
    pthread_mutex_unlock(&disconnectLock);
    printf("disconencted\n");
    
    pthread_mutex_lock(&tokenLock);
    if(haveToken == TRUE){
    //We have the token message and we need to pass it one more time.
    pthread_t send_token_thread;
    pthread_create(&send_token_thread, NULL, delaySendToken, NULL);
    }
    pthread_mutex_unlock(&tokenLock);
    
    // Cleanup all connections
    for (i=0; i<nConnected; i++) {
        // if disconnected previously, don't try to do so again
        if (cs[i] != -1) {
            shutdown(cs[i], SHUT_RDWR);
        }
    }
    shutdown(mys, SHUT_RDWR);
    return 1;
}

/* 
 * sets up a local socket to connect to socket at given port number. Currently 
 * connects to given point on local machine, but could connect to distant computer
 * by changing the IP address. 
 * argument: port number to attempt to connect to
 * return: -1 on error, 0 on port not found, socket id on successful connection 
 */ 
int joinNetwork(int port) {
    // Create a socket of type stream which gives reliable message passing.  
    int s = socket(PF_INET,SOCK_STREAM,0);
    if (s <= 0) {
        printf("client: Socket creation failed.\n");
        return -1;
    }

    // Attempt connection to given port on local machine
    struct sockaddr_in joinAddress;
    int addressSize = sizeof(struct sockaddr_in);
    joinAddress.sin_family=AF_INET;
    joinAddress.sin_port = htons(port); 
    inet_pton(AF_INET,"127.0.0.1",&joinAddress.sin_addr);  // <- IP 127.0.0.1 

    // If connection is successful, connect will retun 0 and set s appropriately
    if (connect(s,(struct sockaddr*) &joinAddress, addressSize) != 0) {
        return 0;
    }
    return s;
}

/*
 * sets up a local socket to accept incoming connections from other chatters
 * argument: port number to accept connections on
 * return: -1 if fail to create socket or bind socket to port, id of created
 * socket otherwise.
 */
int createNetwork(int port) {
    // Create a socket of type stream which gives reliable message passing.  
    int s = socket(PF_INET,SOCK_STREAM,0);
    if (s <= 0) {
        printf("server: Socket creation failed.\n");
        return -1;
    }

    // Create a port to listen to for incoming connections
    addressSize = sizeof(struct sockaddr);
    address.sin_family=AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port  = htons(port); 

    // bind the port to the socket
    int status=0;
    status = bind(s,(struct sockaddr*) &address, addressSize);
    if (status != 0) {
        printf("server: Bind failed, unable to create port.\n");
        return -1;
    }

    // listen for incoming connections on this socket, handle a backlog of up
    // to 3 requests
    listen(s,3);

    return s;
}

/* 
 * Accepts incoming connection request to given socket. accept is a blocking system
 * call so will sleep until request happens, then be woken up to handle it.
 * argument: socket to check for incoming connections on
 * return: -1 if error, id of new connection if successful.
 */
int acceptConnection(int sock) {
    int newChatter = accept(sock,(struct sockaddr*) &address,(socklen_t *) &addressSize);
    if (newChatter <= 0) {
        printf("server: Accept failed, new chatter can't connect to port.\n");
        return -1;
    }
    
    return newChatter;
}

/*
 * continually recieves messages on one connected socket. recv is a blocking system call 
 * so will sleep until incoming message appears, then be woken up to handle it.
 * argument: index into cs of this connection. conversion to long then int is
 * required to avoid warnings, and I know some of you are very bothered by warnings =)
 */
void* listenSocket(void* args) {
    char* buffer = (char *) malloc(BUFFER_SIZE);
    int index = (int)(long)args;
    int sock = cs[index];
    int size;
    
    // recv blocks until message appears. 
    // returns size of message actually read, or 0 if realizes connection is lost
    size = recv(sock, buffer, BUFFER_SIZE, 0);
    while (size>0) {
        if (buffer[0] == 'P') { //A connection is sending us its port number.
            char portNumStr[BUFFER_SIZE];
            strcpy(portNumStr, &buffer[1]);
            int newPortNum = atoi(portNumStr);
            if (newPortNum >= 1100 && newPortNum <= 1200) {
                cp[index] = newPortNum;
                updateTokenChain(index);
            }
        } else if (buffer[0] == 'T') { //We are receiving the token.
            pthread_mutex_lock(&tokenLock);
            haveToken = TRUE;
            pthread_mutex_unlock(&tokenLock);
            char tokenNumStr[BUFFER_SIZE];
            strcpy(tokenNumStr, &buffer[1]);
            int receivedTokenNum = atoi(tokenNumStr);
            if (receivedTokenNum <= 0) {
                printf("Received a malformatted token-passing message; the chain is compromised. womp. womp.\n");
                return;
            }
            if (initializedTokenNum == FALSE) {
                initializedTokenNum = TRUE;
                tokenNum = receivedTokenNum;
                printf("Ready to chat!\n");
            }
            if (tokenNum == receivedTokenNum) { //All the messages we need are now in order
                if (waitingForExpectedMessages == TRUE) {
                    waitingForExpectedMessages = FALSE;
                }
                pthread_t send_thread;
                pthread_create(&send_thread, NULL, sendBufferedMessages, NULL);
            } else if (tokenNum < receivedTokenNum) { //Still missing some messages
                waitingForExpectedMessages = TRUE;
                lastExpectedMessageValue = receivedTokenNum % 256;
            }
        } else if (buffer[0] == 'M') { //We are receiving a regular user-to-user message.
            int receivedTokenNum = (int) buffer[1];
            strcpy(buffer, &buffer[2]);
            if (initializedTokenNum != FALSE) {
                numReceived[index] += 1;
                fflush(stdout);
                processReceivedMessage(sock, receivedTokenNum, buffer);
            }
        } else if (buffer[0] == 'C') { //We received confirmation of our disconnection; okay to quit.
            pthread_cond_signal(&disconnectCondition);
        } else if (buffer[0] == 'Q') { //We received notification of another chatter leaving the system; we only care if they were our tokenPassingBuddy
            printf("Client %d disconnected from the chat.\n", cs[index]);
            if (index == tokenPassingBuddyIndex) {
                // Our buddy has left, find a new buddy.
                char buddyPortStr[BUFFER_SIZE];
                strcpy(buddyPortStr, &buffer[1]);
                int newBuddyPortNum = atoi(buddyPortStr);
                mustSendConfirmation = TRUE;
                confirmationSocket = cs[index];
                cp[index] = -1;
                if (nConnected == 1) { //The last client disconnected.
                    int tokenNum = 1;
                    int initializedTokenNum = FALSE;
                    mustStartTokenPassing = TRUE;
                } else if (updateBrokenTokenChain(newBuddyPortNum) != 0) {
                    printf("Failed to find new token passing buddy as suggested by our old one; the system is compromised. womp. womp.\n");
                }
            }
        }
        size = recv(sock, buffer, BUFFER_SIZE, 0);
     }

    // socket was closed by other side so close this end as well
    shutdown(cs[index], SHUT_RDWR);
    cs[index] = -1;
 }
 
 //Determines we received the next message that we needed
void processReceivedMessage(int sock, int receivedTokenNum, char* buffer) {
    receivedBuffer_add((256*(tokenNum / 256)) + receivedTokenNum, buffer, sock);
    if (receivedTokenNum == (tokenNum % 256) + 1) { //This is the next token we expected
        printBuffer();
        if (waitingForExpectedMessages == TRUE &&
              lastExpectedMessageValue == (tokenNum % 256)) {
            waitingForExpectedMessages = FALSE;
            pthread_t send_thread;
            pthread_create(&send_thread, NULL, sendBufferedMessages, NULL);
        }
    }
}

//Prints off all of the messages in the buffer that are ordered correctly
void printBuffer() {
    while (receivedBufferHead != NULL && receivedBufferHead->tokenNum == (tokenNum % 256) + 1) {
        tokenNum++;
        printf("%d: %s\n", receivedBufferHead->clientNum, receivedBufferHead->message);
        receivedMessage toDel = receivedBuffer_pop();
    }
}

/*
 * continually get input from this chat user and add it to the send buffer.
 * also does some formatting to make terminal look more like typical chat window to user.
 * return 1 if user enters command to quit, 0 otherwise.
 */
int getAndSend() {
    char* buffer = (char *) malloc(BUFFER_SIZE);

    // Get keyboard input from user, strip newline, quit if !q
    ssize_t nChars = getline(&buffer, &BUFFER_SIZE, stdin);
    buffer[nChars-1] = '\0';
    if (strcmp(buffer,"!q") == 0) {
        return 1;
    }

    // clear line user just entered (only want to display lines when we choose in case need to reorder)
    printf("\33[1A\r");
    printf("\33[2K\r");

    // check that sendBuffer is not full
    pthread_mutex_lock(&sendBufLock);
    if (sendBuffer.nHeld == 10) {
        pthread_mutex_unlock(&sendBufLock);
        printf("\n\nERROR: send buffer full, undefined behavior for this case, exiting instead.\n\n");
        exit(1);
    }
    
    // copy message to send buffer at appropriate index and initalize num remaining to send to 0
    int index = (sendBuffer.startIndex+sendBuffer.nHeld)%10;
    sendBuffer.messages[index].nToSend = 0;
    strcpy(sendBuffer.messages[index].message, buffer);
    sendBuffer.nHeld++;
    pthread_mutex_unlock(&sendBufLock);
    
    return 0;
}

//Sends on all messages in the buffer when we acquire the token
void* sendBufferedMessages(void* args) {    
    pthread_mutex_lock(&sendBufLock);
    int numToSend = sendBuffer.nHeld; // Get the number of messages to send
    //We will ignore any messages added to the send buffer while in the process of sending out messages
    pthread_mutex_unlock(&sendBufLock);
    
    
    int j;
    for (j=0; j < numToSend; j++) {
        pthread_mutex_lock(&sendBufLock);
        int index = (sendBuffer.startIndex + j) % 10;
        // print line in my own window
        printf("Me: %s\n", sendBuffer.messages[index].message);
        strcpy(&sendBuffer.messages[index].message[2], sendBuffer.messages[index].message);
        tokenNum++;
        sendBuffer.messages[index].message[0] = 'M';
        sendBuffer.messages[index].message[1] = (char) tokenNum;
        pthread_mutex_unlock(&sendBufLock);
        
        int i;
        // Send message to all connections except those that have already been closed
        for (i=0; i<nConnected; i++) {
            if (cs[i] != -1) {
     
                // increment num remaining to send of this message
                pthread_mutex_lock(&sendBufLock);
                sendBuffer.messages[index].nToSend++;
                pthread_mutex_unlock(&sendBufLock);
                
                // create thread to do the actual sending so can add delays, needs to know
                // index of message in sendBuffer and socket to send to. Totally cheating and using the fact
                // that a void* is 64 bits and each of these args is 32 bits so simply putting the index in 
                // the 1st 32 bits of args and the socket in the 2nd 32 bits. Note that something seemingly 
                // more logical like creating an array of the 2 integers and passing the address won't work 
                // because the array will only exist until this function ends, before the threads actually 
                // need to use it.
                long indexL = (long)index;
                void* args = (void*)((indexL << 32) + cs[i]);
                pthread_t send_thread;
                pthread_create(&send_thread, NULL, delaySend, args);
             }
        }
    }
    
    //If we need to send a confirmation to our token buddy acknowledging their leaving, now is the time to do it
    if (mustSendConfirmation == TRUE) {
        mustSendConfirmation = FALSE;
        pthread_t send_disconnect_confirmation_thread;
        pthread_create(&send_disconnect_confirmation_thread, NULL, delaySendDisconnectConfirm, NULL);
    }
    
    //Send the token message
    pthread_t send_token_thread;
    pthread_create(&send_token_thread, NULL, delaySendToken, NULL);
    pthread_mutex_lock(&tokenLock);
    haveToken = FALSE;
    pthread_mutex_unlock(&tokenLock);
}

/* 
 * thread to sleep for random amount of time between 0 and given max # seconds, then send message.
 * update sendBuffer as appropriate
 * argument: first 32 bits of void* is index of message in sendBuffer, last 32 bits give 
 * socket to send message to.
 */
void* delaySend(void* args) {
    // reversing process above to get index and socket out of args
    long argsL = (long)args;
    int index = (int)(argsL >> 32);
    int socket = (int)((argsL << 32) >> 32);
    
    // delay random amount up to max allowed
    usleep(rand()%(maxDelay*1000000));
    
    pthread_mutex_lock(&sendBufLock);
    send(socket, sendBuffer.messages[index].message, BUFFER_SIZE, 0);

    // once send has completed, decrement num remaining to send of this message, if this was the last
    // "remove" this message from send buffer (actually just move startIndex and decrement nHeld)
    sendBuffer.messages[index].nToSend--;
    if (sendBuffer.messages[index].nToSend == 0) {
        sendBuffer.startIndex = (sendBuffer.startIndex+1)%10;
        sendBuffer.nHeld--;
    }
    pthread_mutex_unlock(&sendBufLock);
}

/**
 * Send the token message with a delay; the token message should be set prior to calling this message.
 * Uses the stored port number for our token-passing buddy as the target for this message.
 */
void* delaySendToken(void* args) {
    
    char message[1024];
    sprintf(message, "T%d", tokenNum);
    
    
    // delay random amount up to max allowed
    usleep(rand()%(maxDelay*1000000));
    
    send(cs[tokenPassingBuddyIndex], message, BUFFER_SIZE, 0);
}

/*
 Sends our port number to the given socket, for handshaking when a chatter joins
 This is used by the recipient to determine if we are a better match than their current
 token buddy.
*/
void* delaySendPortNum(void* args) {
    // reversing process above to get index and socket out of args
    long argsL = (long)args;
    int useless = (int)(argsL >> 32);
    int socket = (int)((argsL << 32) >> 32);
    
    char message[1024];
    sprintf(message, "P%d", portNum);
        
    // delay random amount up to max allowed
    usleep(rand()%(maxDelay*1000000));
    
    send(socket, message, BUFFER_SIZE, 0);
}

/*
 * Sends a disconnect message along with our own port number to another chatter.
 * If the chatter passes the token to us, they will use this to reset their
 * connection.
 */
void* delaySendDisconnect(void* args) {
    // reversing process above to get index and socket out of args
    long argsL = (long)args;
    int useless = (int)(argsL >> 32);
    int socket = (int)((argsL << 32) >> 32);
    
    char message[1024];
    sprintf(message, "Q%d", cp[tokenPassingBuddyIndex]);
        
    // delay random amount up to max allowed
    usleep(rand()%(maxDelay*1000000));
    
    send(socket, message, BUFFER_SIZE, 0);
}

/*
 * If  our token passing buddy sends us notice that they are disconnecting,
 * confirm to them that we have reset our token passing and that they can
 * safely leave.
 */
void* delaySendDisconnectConfirm(void* args) {
    
    char message[1024];
    sprintf(message, "C");
    
    
    // delay random amount up to max allowed
    usleep(rand()%(maxDelay*1000000));
    
    send(confirmationSocket, message, BUFFER_SIZE, 0);
    confirmationSocket = -1;
}

/*
 * wait for incoming connections. When connection is made, save to next slot in connected
 * socket array (cs) and create a new receiver thread to listen to new chatter. Note that main
 * thread will automatically start sending to this new chatter as well because of updated
 * nConnected. acceptConnection function makes a blocking call so we don't need to worry about
 * busy waiting.
 * argument is socket id to look for incoming requests on. conversion to long then int is
 * required to avoid warnings, and I know some of you are very bothered by warnings =) 
 */
void* acceptIncoming(void* args) {
    int sock = (int)(long)args;
    while (1==1) {
        int newC = acceptConnection(sock);
        if (newC == -1) exit(1);
        cs[nConnected] = newC;
        cp[nConnected] = -1;
        pthread_create(&receivers[nConnected], NULL, listenSocket, (void*)(long)nConnected);
        nConnected++;
    }
}

/* 
 * scan all ports from 1100 to 1200 for already existing chatters by attempting
 * to connect to each. If successfully connect to a port, save connection in next slot 
 * in connected socket array (cs), otherwise do nothing.
 * return -1 if encounter failure in creating socket for connection on this end
 */
int connectCurrent() {
    int i;
    for (i=1100; i<1200; i++) {
        int s = joinNetwork(i);
        if (s == -1) return -1;
        if (s > 0) {
            printf("connected to socket at port %d\n", i);
            cs[nConnected] = s;
            cp[nConnected] = i;
            updateTokenChain(nConnected);
            nConnected++;
        }
    }
    return 0;
}

/*
 * When a new chatter joins, determine whether to pass the token to them in the future.
 * Since this is called by all chatters whenever a new chatter joins, the ring
 * will be reset correctly.
 */
void updateTokenChain(int newConnectionIndex) {
    int oldBuddy = tokenPassingBuddyIndex;
    if (tokenPassingBuddyIndex == -1) {
        tokenPassingBuddyIndex = newConnectionIndex;
    } else { //We already have a token-passing buddy
        int oldBuddyPortNum = cp[tokenPassingBuddyIndex] % 100;
        int newBuddyPortNum = cp[newConnectionIndex] % 100;
        int localPortNum = portNum % 100;
        if (oldBuddyPortNum < localPortNum) { //We are passing backwards
            if (newBuddyPortNum > localPortNum) { //Found a port to pass to ahead of us
                tokenPassingBuddyIndex = newConnectionIndex;
            } else if (newBuddyPortNum < oldBuddyPortNum) { //Pass even farther backwards
                tokenPassingBuddyIndex = newConnectionIndex;
            }
        } else { //We are passing forward
            if (newBuddyPortNum < oldBuddyPortNum) { //A less-forward port to pass to
                tokenPassingBuddyIndex = newConnectionIndex;
            }
        }
    }
    
    if (mustStartTokenPassing == TRUE && tokenPassingBuddyIndex != -1) {
        //Send the token message
        pthread_t send_token_thread;
        pthread_create(&send_token_thread, NULL, delaySendToken, NULL);
        mustStartTokenPassing = FALSE;
    }

}

/*
 *When this chatter's token target departs, reset the token chain properly
 */
int updateBrokenTokenChain(int portNum) {
    int i;
    for (i = 0; i < 100; i++) {
        if (cp[i] == portNum) {
            tokenPassingBuddyIndex = i;
            return 0;
        } else{
        }
    }
    return 1; //Couldn't find who was supposed to be our new buddy
}

/*
 * We have a linked list buffer in sorted order of received messages. This
 * inserts a new message into the correct place.
 */
void receivedBuffer_add(int receivedTokenNum, char* buffer, int sock) {
    pthread_mutex_lock(&receivedBufLock);
    receivedMessage *newMsg = (struct receivedMessage *) malloc(sizeof(receivedMessage));
    newMsg->tokenNum = receivedTokenNum;
    newMsg->clientNum = sock;
    strcpy(newMsg->message, buffer);
    
    if (receivedBufferHead == NULL) {
        receivedBufferHead = newMsg;
    } else { //Other messages in the received buffer
        if (receivedBufferHead->tokenNum > newMsg->tokenNum) {
            newMsg->next = receivedBufferHead;
            receivedBufferHead = newMsg;
        } else { //Need to dig into the linked list
            receivedMessage *prev = NULL;
            receivedMessage *curr = receivedBufferHead;
            while (curr != NULL) {
                if (curr->tokenNum > newMsg->tokenNum) {
                    prev->next = newMsg;
                    newMsg->next = curr;
                    break;
                }
                if (curr->next == NULL){ //We have reached the end of the list and need to insert
                    curr->next = newMsg;
                    newMsg->next = NULL;
                    break;
                }
                prev = curr;
                curr = curr->next;
            }
        }
    }
    pthread_mutex_unlock(&receivedBufLock);
}

/*
 * removes the head of the buffer and returns it.
 */
struct receivedMessage receivedBuffer_pop() {
    pthread_mutex_lock(&receivedBufLock);
    receivedMessage firstMessage = *receivedBufferHead;
    receivedBufferHead = receivedBufferHead->next;
    pthread_mutex_unlock(&receivedBufLock);
    return firstMessage;
}

