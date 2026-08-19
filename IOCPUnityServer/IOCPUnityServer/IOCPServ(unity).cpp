#include <iostream>
#include <mutex>
#include <stdlib.h>
#include <process.h>
#include <winsock2.h>
#include <windows.h>
#include <thread>
#include <vector>


#define BUF_SIZE 100
#define READ	3
#define	WRITE	5
#define MAX_CLNT 100
constexpr int PORT_NUMBER = 9190;

#define LPPER_HANDLE_DATA PER_HANDLE_DATA*
#define LPPER_IO_DATA PER_IO_DATA*

/**
* C# 클라이언트(유니티)와 연동하는 C++ IOCP 서버입니다.
* 
* 유니티에서 자동적으로 "[닉네임] : 채팅내용" 으로 변환되어 서버에 전송되기에, '['로 시작하는 메세지만 접속한 모든 클라이언트에 전송합니다.
*/


class PER_HANDLE_DATA
{
public:
    SOCKET hClntSock;
    SOCKADDR_IN clntAdr;
};

class PER_IO_DATA
{
public:
    OVERLAPPED overlapped;
    WSABUF wsaBuf;
    char buffer[BUF_SIZE];
    int rwMode; // READ or WRITE


    PER_IO_DATA()
    {
        memset(&overlapped, 0, sizeof(OVERLAPPED));
        wsaBuf.len = BUF_SIZE;
        wsaBuf.buf = (char*)buffer;
        rwMode = READ;
    };
};


DWORD WINAPI ThreadMain(LPVOID CompletionPortIO);
void ErrorHandling(const char* message);

int clntCnt = 0;
SOCKET clntSockets[MAX_CLNT];
std::mutex clntMutex;


int main()
{
    WSADATA wsaData;
    HANDLE hComPort;
    SYSTEM_INFO sysInfo;
    LPPER_IO_DATA ioInfo;
    LPPER_HANDLE_DATA handleInfo;

    SOCKET hServSock;
    SOCKADDR_IN servAdr;
    int recvBytes, i, flags = 0;

    //윈속을 초기화합니다.
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        ErrorHandling("WSAStartup() error!");

    //CP 오브젝트를 생성합니다
    hComPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    GetSystemInfo(&sysInfo);

    //스레드를 생성합니다.
    for (i = 0; i < sysInfo.dwNumberOfProcessors; i++)
        _beginthreadex(NULL, 0, (_beginthreadex_proc_type)ThreadMain, (HANDLE)hComPort, 0, NULL);

    //비동기 Accept를 위한 소켓을 생성합니다.
    hServSock = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
    memset(&servAdr, 0, sizeof(servAdr));
    servAdr.sin_family = AF_INET;
    servAdr.sin_addr.s_addr = htonl(INADDR_ANY);
    servAdr.sin_port = htons(PORT_NUMBER);


    //바인딩 및 리슨 상태로 전환합니다.
    bind(hServSock, (SOCKADDR*)&servAdr, sizeof(servAdr));
    listen(hServSock, 5);

    while (1)
    {
        SOCKET hClntSock;
        SOCKADDR_IN clntAdr;
        int addrLen = sizeof(clntAdr);

        //accept
        hClntSock = accept(hServSock, (SOCKADDR*)&clntAdr, &addrLen);

        //(뮤텍스) 접속중인 소켓 배열에 추가합니다.
        {
            std::lock_guard<std::mutex> lock(clntMutex);
            clntSockets[clntCnt++] = hClntSock;
        }

        //핸들 객체 생성 및 입력합니다.
        handleInfo = (LPPER_HANDLE_DATA)new PER_HANDLE_DATA;
        handleInfo->hClntSock = hClntSock;
        memcpy(&(handleInfo->clntAdr), &clntAdr, addrLen);

        //CP 오브젝트와 소켓을 연결합니다.
        CreateIoCompletionPort((HANDLE)hClntSock, hComPort, (ULONG_PTR)handleInfo, 0);

        // 객체를 생성합니다.
        ioInfo = (LPPER_IO_DATA)new PER_IO_DATA;


        //recv 
        WSARecv(handleInfo->hClntSock, &(ioInfo->wsaBuf),
                1, (LPDWORD)&recvBytes, (LPDWORD)&flags, &(ioInfo->overlapped), NULL);
    }
    return 0;
}

//입출력용 스레드
DWORD WINAPI ThreadMain(HANDLE pComPort)
{
    HANDLE hComPort = (HANDLE)pComPort;
    SOCKET sock;
    DWORD bytesTrans;
    LPPER_HANDLE_DATA handleInfo;
    LPPER_IO_DATA ioInfo;
    DWORD flags = 0;

    while (1)
    {
        //호출시 대기스레드 큐에 들어갔다가, IO가 완료되면 LIFO로 큐의 스레드를 깨워서 데이터를 전달받습니다.
        GetQueuedCompletionStatus(hComPort, &bytesTrans,
                                  (PULONG_PTR)&(handleInfo), (LPOVERLAPPED*)&ioInfo, INFINITE);
        sock = handleInfo->hClntSock;

        //recv일시
        if (ioInfo->rwMode == READ)
        {
            std::cout << "Message Recieved!\n";
            if (bytesTrans == 0) // EOF 전송 시
            {
                std::cout << "CloseSocket\n";
                //(뮤텍스)접속중인 소켓 배열에서 삭제
                {
                    std::lock_guard<std::mutex> lock(clntMutex);
                    for (int i = 0; i < clntCnt; i++)
                    {
                        if (clntSockets[i] == sock)
                        {
                            while (i < clntCnt - 1)
                            {
                                clntSockets[i] = clntSockets[i + 1];
                                i++;
                            }
                            break;
                        }
                    }
                    clntCnt--;
                }
                closesocket(sock);
                delete handleInfo;
                delete ioInfo;
                continue;
            }

            ioInfo->wsaBuf.len = bytesTrans;
            ioInfo->rwMode = WRITE;

            //(채팅 :시작이 '['일 경우)모든 클라이언트에게 메시지 전송
            if (ioInfo->wsaBuf.buf[0] == (byte)'[')
            {
                // (뮤텍스) 브로드캐스트 도중 배열이 바뀌는 것을 방지하기 위해 스냅샷 적용 　				
                std::vector<SOCKET> targets;
                {
                    std::lock_guard<std::mutex> lock(clntMutex);
                    targets.assign(clntSockets, clntSockets + clntCnt); // 복사만
                }


                for (SOCKET clientSocket : targets)
                {
                    //수신자마다 PER_IO_DATA를 가지도록 함							
                    LPPER_IO_DATA sendInfo = new PER_IO_DATA;
                    memcpy(sendInfo->buffer, ioInfo->buffer, bytesTrans);
                    sendInfo->wsaBuf.len = bytesTrans;
                    sendInfo->rwMode = WRITE;
                    WSASend(clientSocket, &(sendInfo->wsaBuf), 1, NULL, 0,
                            &(sendInfo->overlapped), NULL);
                }
            }

            //전송 후 ioInfo 초기화
            ioInfo = (LPPER_IO_DATA)new PER_IO_DATA;

            //READ상태로 변경 및 recv
            ioInfo->rwMode = READ;
            WSARecv(sock, &(ioInfo->wsaBuf),
                    1, NULL, &flags, &(ioInfo->overlapped), NULL);
        }
        //다른 스레드에서 send시
        else
        {
            std::cout << "Message Sent!\n";
            delete ioInfo;
        }
    }
    return 0;
}

void ErrorHandling(const char* message)
{
    fputs(message, stderr);
    fputc('\n', stderr);
    exit(1);
}
