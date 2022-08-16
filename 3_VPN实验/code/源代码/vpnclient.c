#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <sys/ioctl.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <netdb.h>
#include <termios.h> //��������
#include <signal.h>  //���� ctrl+c �źŲ������˷�����ָֹ��

/* define HOME to be dir for key and cert files... */
// ֤���Ŀ¼
#define HOME "./cert_server/"
/* Make these what you want for cert & key files */

#define CACERT HOME"ca.crt"

#define CHK_NULL(x)	if ((x)==NULL) exit (1)
#define CHK_ERR(err,s)	if ((err)==-1) { perror(s); exit(1); }
#define CHK_RET(err,s)	if ((err)!=0) { perror(s); exit(3); }
#define CHK_SSL(err)	if ((err)==-1) { ERR_print_errors_fp(stderr); exit(2); }

#define PORT_NUMBER 4433
#define BUFF_SIZE 2000  
#define HOSTNAME "chengziServer.com"


//֤����֤
int verifyCallback(int preverify_ok, X509_STORE_CTX *x509_ctx) {
    char buf[300];
    X509 *cert = X509_STORE_CTX_get_current_cert(x509_ctx);
    X509_NAME_oneline(X509_get_subject_name(cert), buf, 300);
    printf("certificate subject= %s\n", buf);

    if (preverify_ok == 0) {
        int err = X509_STORE_CTX_get_error(x509_ctx);
        printf("Verification failed: %s.\n",
               X509_verify_cert_error_string(err));
        return 0;   //����0����TLS��������
    }
    printf("Verification passed.\n");
    return 1;   //����1����TLS����
}

//�������������豸
//����������һ���豸�Լ�һ��TCP�׽���
int createTunDevice(int virtualIP) {
    int tunfd;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));

    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    //IFF_TUN:��ʾ����һ��TUN�豸
    //IFF_NO_PI:��ʾ��������ͷ��Ϣ

    //��TUN�豸
    tunfd = open("/dev/net/tun", O_RDWR);
    if (tunfd == -1) {
        printf("Open /dev/net/tun failed! (%d: %s)\n", errno, strerror(errno));
        return -1;
    }
    //ע���豸����ģʽ
    int ret = ioctl(tunfd, TUNSETIFF, &ifr);
    if (ret == -1) {
        printf("Setup TUN interface by ioctl failed! (%d: %s)\n", errno, strerror(errno));
        return -1;
    }
    printf("Create a tun device :%s\n", ifr.ifr_name);
    //�����豸���
    int tunId = atoi(ifr.ifr_name+3);

    char cmd[60];
    //������IP�󶨵�TUN�豸��
    sprintf(cmd,"sudo ifconfig tun%d 192.168.53.%d/24 up",tunId, virtualIP);
    system(cmd);
    //�����͸�192.168.60.0/24�����ݰ�����TUN�豸����
    sprintf(cmd,"sudo route add -net 192.168.60.0/24 dev tun%d",tunId);
    system(cmd);
    return tunfd;
}

//��ʼ��TLS�ͻ���
SSL *setupTLSClient(const char *hostname) {
    // Step 0: OpenSSL library initialization
    // This step is no longer needed as of version 1.1.0.
    // ��һ������ʼ��OpenSSL��
    SSL_library_init();
    SSL_load_error_strings();
    SSLeay_add_ssl_algorithms();

    // �ڶ�����SSL�����ĳ�ʼ��
    SSL_METHOD *meth = (SSL_METHOD *)SSLv23_client_method();
    //�����ỰЭ��
    SSL_CTX *ctx = SSL_CTX_new(meth);
    if (!ctx) {
        ERR_print_errors_fp(stderr);
        // exit(2);
    }
    
    // ������������֤����֤����
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, verifyCallback);
    //����֤��
    if (SSL_CTX_load_verify_locations(ctx, CACERT, NULL) < 1)  {
        printf("Error setting the verify locations. \n");
        exit(0);
    }

    SSL *ssl = SSL_new(ctx);
    X509_VERIFY_PARAM *vpm = SSL_get0_param(ssl);
    X509_VERIFY_PARAM_set1_host(vpm, hostname, 0);
    SSL_CTX_free(ctx);
    return ssl;
}


//��ʼ��TCP�ͻ���
int setupTCPClient(const char *hostname, int port) {
    struct sockaddr_in serverAddr;

    // ��������ȡIP��ַ
    struct hostent *hp = gethostbyname(hostname);

    // ����TCP�׽���
    int sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    CHK_ERR(sockfd,"socket");

    // ���������Ϣ(IP, �˿ں�, Э����)
    memset(&serverAddr, '\0', sizeof(serverAddr));
    memcpy(&(serverAddr.sin_addr.s_addr), hp->h_addr, hp->h_length);
    //   server_addr.sin_addr.s_addr = inet_addr ("10.0.2.14");
    serverAddr.sin_port = htons(port);
    serverAddr.sin_family = AF_INET;

    // �����˽�������
    connect(sockfd, (struct sockaddr *)&serverAddr, sizeof(serverAddr));
    printf("TCP connect succeed! hostname IP:%s port:%d\n", inet_ntoa(serverAddr.sin_addr), port);
    return sockfd;
}


int mygetch() {
    struct termios oldt, newt;
    int ch;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    ch = getchar();
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    return ch;
}

//�������뺯��
int getPasswd(char *passwd, int size) {
    int c, n = 0;
    do {
        c = mygetch();
        if (c != '\n' && c != '\r' && c != 127) {
            passwd[n] = c;
            printf("*");
            n++;
        } else if ((c != '\n' | c != '\r') && c == 127) { //�ж��Ƿ��ǻس������˸�
            if (n > 0) {
                n--;
                printf("\b \b"); //����˸�
            }
        }
    } while (c != '\n' && c != '\r' && n < (size - 1));
    passwd[n] = '\0'; //����һ������Ļس�
    putchar('\n');
    return n;
}

//�ͻ�����֤
int verifyClient(SSL *ssl) {
    char username[20];
    char passwd[20];
    char recvBuf[BUFF_SIZE];
    int len = SSL_read(ssl,recvBuf,BUFF_SIZE);
    
    //�����û���
    printf("%s\n",recvBuf);
    scanf("%s", username);
    getchar();
    SSL_write(ssl,username,strlen(username)+1);
    //��������
    SSL_read(ssl,recvBuf,BUFF_SIZE);
    printf("%s\n",recvBuf);
    getPasswd(passwd, 20);
    SSL_write(ssl,passwd,strlen(passwd)+1);
    //��ȡ��֤���
    SSL_read(ssl,recvBuf,BUFF_SIZE);

    if(strcmp(recvBuf, "Client verify succeed") != 0) {
        printf("Client verify failed!\n");
        return -1;
    }
    printf("Client verify succeed\n");
    return 1;
}

void sendRequest(SSL* ssl) {
    char msg[]="Hello VPN!";
    int len = SSL_write(ssl,msg,strlen(msg)+1);
    CHK_SSL(len);
    char recvBuf[BUFF_SIZE];
    len = SSL_read(ssl,recvBuf,BUFF_SIZE);
    CHK_SSL(len);
    printf("Got %d bytes: %s\n",len,recvBuf);
}

//��TLS�����������
//TUN���ݾ���,�����ݴ�TUNд���׽��ֽ��з���
void tunSelected(SSL* ssl, int tunfd) {
    int len;
    char buff[BUFF_SIZE];
    bzero(buff, BUFF_SIZE);

    len = read(tunfd, buff, BUFF_SIZE); //��TUN�豸�ж�ȡ����
    buff[len] = '\0';
    printf("len %4d, Got a packet from TUN\n", len);

    SSL_write(ssl, buff, len);  //������д�뵽�׽�����
}


// ��TLS����������ݣ�ע�ⳤ��Ϊ0ʱ����رգ�Ҫֹͣ����/�߳�
int socketSelected(SSL* ssl, int tunfd)
{
	int len;
	char buff[BUFF_SIZE];

	bzero(buff, BUFF_SIZE);
	// ���׽����ж�ȡ����
	len = SSL_read(ssl, buff, BUFF_SIZE - 1);
	
	printf("len %4d, Got a packet from the tunnel\n", len);
	
	if(len == 0) {    // �������ر�
    return 1;
  }
  buff[len] = '\0';
	
	//������д��TUN�豸
	write(tunfd, buff, len);
	return 0;
}

//select�����׽��ֺ������豸
void selectTunnel(SSL* ssl, int sockfd, int tunfd) {
    while (1) {
        //ʹ��select����IO��·���ü����׽��ֺ������豸
        fd_set readFDSet;           //��ȡ�ļ���������
        FD_ZERO(&readFDSet);        //���ļ������������
        FD_SET(sockfd, &readFDSet); //���׽������������뼯��
        FD_SET(tunfd, &readFDSet);  //���豸�˿ڼ��뼯��
        select(FD_SETSIZE, &readFDSet, NULL, NULL, NULL);
        //������TUN�豸����
        if (FD_ISSET(tunfd, &readFDSet)) {
            tunSelected(ssl, tunfd);
        }
        //�������׽��־���
        if (FD_ISSET(sockfd, &readFDSet)) {
            if(socketSelected(ssl, tunfd) == 1){
                printf("VPN Server Closed\n");
                return;
            }
        }
    }
}

//��ȡ����˷��������IP
int recvVirtualIP(SSL* ssl) {
    char buf[10];
    SSL_read(ssl,buf,BUFF_SIZE);
    int virtualIP = atoi(buf);
    printf("virtualIP: 192.168.53.%d/24\n",virtualIP);
    return virtualIP;
}


int main(int argc, char *argv[]) {
    /*----------------TLS initialization ----------------*/
    SSL *ssl = setupTLSClient(HOSTNAME);

    /*----------------Create a TCP connection ---------------*/
    int sockfd = setupTCPClient(HOSTNAME, PORT_NUMBER);

    /*----------------TLS handshake ---------------------*/
    SSL_set_fd(ssl, sockfd); 
    int err = SSL_connect(ssl);
    if(err <= 0) {
        printf("SSL_connect failed!\n");
        close(sockfd);
        return 0;
    }

    printf("SSL connection is successful\n");
    printf("SSL connection using %s\n", SSL_get_cipher(ssl));

    /*----------------Verify client ---------------------*/
    if (verifyClient(ssl) != 1) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
        exit(2);
    }

    // sendRequest(ssl);

    /*----------------Receive Virtual IP ---------------------*/
    int virtualIP = recvVirtualIP(ssl);

    /*----------------Create TUN device ---------------------*/
    int tunfd = createTunDevice(virtualIP);

    /*----------------Send/Receive data --------------------*/
    selectTunnel(ssl,sockfd,tunfd);

    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(sockfd);
    return 0;
}