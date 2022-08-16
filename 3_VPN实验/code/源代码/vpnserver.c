#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <sys/ioctl.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <netdb.h>
#include <shadow.h>
#include <crypt.h> //client verify
#include <memory.h>
#include <pthread.h>

#define PORT_NUMBER 4433
#define BUFF_SIZE 2000

/* define HOME to be dir for key and cert files... */
#define HOME "./cert_server/"

/* Make these what you want for cert & key files */
#define CERTF   HOME"server.crt"
#define KEYF    HOME"server.key"
#define CACERT  HOME"ca.crt"

#define CHK_NULL(x)	if ((x)==NULL) exit (1)
#define CHK_ERR(err,s)	if ((err)==-1) { perror(s); exit(1); }
#define CHK_RET(err,s)	if ((err)!=0) { perror(s); exit(3); }
#define CHK_SSL(err)	if ((err)==-1) { ERR_print_errors_fp(stderr); exit(2); }

struct sockaddr_in peerAddr;
pthread_mutex_t mutex_tun;
SSL_CTX* ctx;


// TLSЭ�̵�׼������
SSL_CTX* initTLS() {
    SSL_METHOD *meth;
    SSL_CTX *ctx;

    // ��һ������ʼ��OpenSSL��
	// This step is no longer needed as of version 1.1.0.
    SSL_library_init();             //ʹ��OpenSSLǰ��Э���ʼ������ 
    SSL_load_error_strings();       //���ش�������ƣ���ӡ��һЩ�����Ķ��ĵ�����Ϣ
    SSLeay_add_ssl_algorithms();    // ���SSL�ļ���/HASH�㷨

    // �ڶ�����SSL�����ĳ�ʼ��
    meth = (SSL_METHOD *)SSLv23_server_method(); //ѡ��ỰЭ��
    //�����ỰЭ��
    ctx = SSL_CTX_new(meth);
    //CHK_RET(ctx,stderr);
     if (!ctx) {
        ERR_print_errors_fp(stderr);
        exit(2);
    }

    //�ƶ�֤����֤��ʽ
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    SSL_CTX_load_verify_locations(ctx, CACERT, NULL);

    // �����������÷�����֤���˽Կ
    // ΪSSL�Ự�����û�֤��
    if (SSL_CTX_use_certificate_file(ctx, CERTF, SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr);
        exit(3);
    }
    // ΪSSL�Ự�����û�˽Կ
    if (SSL_CTX_use_PrivateKey_file(ctx, KEYF, SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr);
        exit(4);
    }
    // ��֤˽Կ��֤���Ƿ����
    if (!SSL_CTX_check_private_key(ctx)) {
        fprintf(stderr, "Private key does not match the certificate public key\n");
        exit(5);
    } else {
        printf("Private key match the certificate public key\n");
    }
    
	return ctx;
}

// ��ʼ��TCP�����
int initTCPServer()
{
	struct sockaddr_in sa_server;
	int listen_sock;

	// ���������׽���
	listen_sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
	CHK_ERR(listen_sock, "socket");
	// �����׽�����Ϣ������Э�顢��ַ���˿�
	memset(&sa_server, '\0', sizeof(sa_server));
	sa_server.sin_family = AF_INET;
	sa_server.sin_addr.s_addr = INADDR_ANY;
	sa_server.sin_port = htons(PORT_NUMBER);
	// ���׽��ֺ��׽�����Ϣ
	int err = bind(listen_sock, (struct sockaddr *) &sa_server, sizeof(sa_server));
	CHK_ERR(err, "bind");
	//�����׽��֣����õȴ����Ӷ��е���󳤶�Ϊ5
	err = listen(listen_sock, 5);
	CHK_ERR(err, "listen");
	
	return listen_sock;
}

// �û���¼����
int userLogin(char *user, char *passwd) {
	//shadow�ļ��Ľṹ�� 
	struct spwd *pw = getspnam(user);    //��shadow�ļ��л�ȡ�����û����ʻ���Ϣ
	if (pw == NULL) return -1;	// û�и��û��򷵻�-1

	printf("Login name: %s\n", user); //�û���¼��
	
	// ��������ܣ���shadow�ļ�������Ա���֤����
	char *epasswd = crypt(passwd, pw->sp_pwdp);
	if (strcmp(epasswd, pw->sp_pwdp)) {
		return -1;
	}
	return 1;
}

// �������������豸/dev/net/tun����������IP�ò�������ȥ
int createTunDevice(int sockfd, int* virtualIP)
{
	int tunfd;
	struct ifreq ifr;
	int ret;

	memset(&ifr, 0, sizeof(ifr));
	// IFF_TUN:��ʾ����һ��TUN�豸��IFF_NO_PI:��ʾ��������ͷ��Ϣ
	ifr.ifr_flags = IFF_TUN | IFF_NO_PI;

	// ϵͳ���Զ���tun�豸�ţ�Ϊ����ͻ��˴�ͬһ���豸���ӻ�����
	pthread_mutex_lock(&mutex_tun);
	tunfd = open("/dev/net/tun", O_RDWR);
	pthread_mutex_unlock(&mutex_tun);
	if (tunfd == -1) {
		printf("Open /dev/net/tun failed! (%d: %s)\n", errno, strerror(errno));
		return -1;
	}
	
	// �����豸�Ľṹ
	ret = ioctl(tunfd, TUNSETIFF, &ifr);
	if (ret == -1) {
		printf("Setup TUN interface by ioctl failed! (%d: %s)\n", errno, strerror(errno));
		return -1;
	}

	// ��õ�ǰ�����豸���
	int tunId = atoi(ifr.ifr_name+3); // ȡ�豸����tunxx��xx��ת��������
	if(tunId == 127){
    printf("[Error] VPN�ͻ��������Ѿ����������ֵ����������޷����ܸ���Ŀͻ��ˡ�");
    return -1;
	}
	
	char cmd[60];
	// �Զ�Ϊ�����������������豸��������IP
	sprintf(cmd, "sudo ifconfig tun%d 192.168.53.%d/24 up", tunId, tunId+1);
	system(cmd);
	// ���ͻ���TUN�ӿڷ�������IP��
	*virtualIP = tunId + 127;  
	// �Զ�Ϊ����������·��
	sprintf(cmd, "sudo route add -host 192.168.53.%d tun%d", tunId+127, tunId);
	system(cmd);
	
	printf("[tunfd %3d] Setup tun%d interface for sockfd %d\n", tunfd, tunId, sockfd);
	return tunfd;
}

// ��TLS�����������
void tunSelected(int tunfd, SSL* ssl)
{
	int len;
	char buff[BUFF_SIZE];

	bzero(buff, BUFF_SIZE);
	len = read(tunfd, buff, BUFF_SIZE);
	buff[len] = '\0';
	
	printf("[tunfd %3d] len %4d, Got a packet from TUN\n", tunfd, len);
	
	// ������д���׽�����
	SSL_write(ssl, buff, len);
}

// ��TLS����������ݣ�ע�ⳤ��Ϊ0ʱ����رգ�Ҫֹͣ�߳�
int socketSelected(int tunfd, SSL* ssl)
{
	int len;
	char buff[BUFF_SIZE];

	bzero(buff, BUFF_SIZE);
	// ���׽����ж�ȡ����
	len = SSL_read(ssl, buff, BUFF_SIZE - 1);
	
	printf("[tunfd %3d] len %4d, Got a packet from the tunnel\n", tunfd, len);
	if(len == 0) {    // �������ر�
    return 1;
  }
  buff[len] = '\0';
	
	//������д��TUN�豸
	write(tunfd, buff, len);
	return 0;
}

//�ͻ�����֤
int verifyClient(SSL *ssl) {
    //��ȡ�û���������
	char iptNameMsg[]="Please input username: ";
    SSL_write(ssl, iptNameMsg, strlen(iptNameMsg)+1);
    char username[BUFF_SIZE];
    int len = SSL_read(ssl, username, BUFF_SIZE);

    char iptPasswdMsg[]="Please input password: ";
    SSL_write(ssl, iptPasswdMsg, strlen(iptPasswdMsg)+1);
    char passwd[BUFF_SIZE];
    len = SSL_read(ssl, passwd, BUFF_SIZE);

    int r = userLogin(username, passwd);
    if(r != 1){
        char no[] = "Client verify failed";
		printf("%s\n",no);
        SSL_write(ssl, no, strlen(no)+1);
		return -1; 
	}
    char yes[] = "Client verify succeed";
    printf("%s\n",yes);
    SSL_write(ssl, yes, strlen(yes)+1);
    return 1;
}


// �ͻ����̴߳�����
void *threadClient(void *sockfdArg){
	int tunfd, sockfd, virtualIP;

	sockfd = (int)sockfdArg; // ����Ĳ�����Server�½���TCP Socket

	/*----------------TLS Э��---------------------*/
    SSL* ssl = SSL_new(ctx);    // �½�SSL�׽���
    SSL_set_fd(ssl, sockfd);    // ��TCP�׽�����SSL�׽��ְ�
    int err = SSL_accept(ssl);  // ʹ��SSL_accept�����������
    if(err <= 0) {
        printf("[Error] SSL_accept ����ʧ��!\n");
        close(sockfd);
        return NULL;
    }

    printf("SSL connection established!\n");
    printf("SSL connection using %s\n", SSL_get_cipher(ssl));

    /*-----Ҫ��ͻ�����֤-----*/
    if (verifyClient(ssl) != 1) {
        //��֤ʧ��ʱ�ر�ssl����
        SSL_shutdown(ssl);
        SSL_free(ssl);
        close(sockfd);
        return NULL;   //�ӽ��̷���
    }

  /* -----����TUN�豸������������IP��----- */
	tunfd = createTunDevice(sockfd, &virtualIP);
	if(tunfd == -1) exit(-1);
	
	/* -----��ͻ��˷�����IP��----- */
	char buf[10];
	sprintf(buf,"%d",virtualIP);
	SSL_write(ssl,buf,strlen(buf)+1);

  /* -----ʹ��select���� IO��·���� �����׽��ֺ������豸----- */
	while (1) {
		fd_set readFDSet;           // ���ļ���������

		FD_ZERO(&readFDSet);        // ���ļ������������
		FD_SET(sockfd, &readFDSet); // ���׽��־�����뼯��
		FD_SET(tunfd, &readFDSet);  // ���豸������뼯��
		select(FD_SETSIZE, &readFDSet, NULL, NULL, NULL);

		// ������Tun�豸����ʱ����VPN�����������
		if (FD_ISSET(tunfd, &readFDSet))
			tunSelected(tunfd, ssl);
		// �������׽����豸����ʱ����VPN�����������
		if (FD_ISSET(sockfd, &readFDSet)){
      if(socketSelected(tunfd, ssl)==1){
        printf("[tunfd %3d] VPN Client Closed\n", tunfd);
        return NULL;
      }
		}
	}
	
	SSL_shutdown(ssl);
	SSL_free(ssl);
	close(sockfd);
	return NULL;
}

int main(int argc, char *argv[])
{
	
	/*-----TLS ��ʼ��-----*/
	ctx = initTLS(); 

	/*-----��ʼ��TCP������׽���-----*/
	int listenSock = initTCPServer();
	
	while (1) {
    /* -----�ȴ��յ��ͻ��˵�TCP����----- */
    struct sockaddr_in clientAddr;
    size_t clientAddrLen = sizeof(struct sockaddr_in);
    int sockfd = accept(listenSock, (struct sockaddr *)&clientAddr, &clientAddrLen);
    CHK_ERR(sockfd, "accept");
    
    printf("Connection from IP:%s port:%d, sockfd: %d\n",
    inet_ntoa(clientAddr.sin_addr), clientAddr.sin_port, sockfd);
    
    /* -----�����ͻ����߳�----- */
    pthread_t tid;
    int ret = pthread_create(&tid, NULL, threadClient, (void*)sockfd);
	if (ret != 0) {close(sockfd);}
	CHK_RET(ret, "pthread ����ʧ��");
  }
	
}
