// provides structs for FB info
#include <linux/fb.h>
// mamory maped files
#include <sys/mman.h>
// ioctls for getting/setting FB settings
#include <sys/ioctl.h>
#include <stdio.h>
// uint8_t and the like
#include <stdint.h>
// constants used with ioctl
#include <fcntl.h>
// memcpy
#include <string.h>

// network
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>

int serverHandle;

// returns server handle
int serverInit(unsigned int port) {
	struct sockaddr_in6 serverAddress;
	
	// get a socket handle
	int serverHandle = socket(AF_INET6, SOCK_STREAM, 0);
	if (serverHandle < 0) {
		fprintf(stderr, "Error getting socket\n");
		exit(EXIT_FAILURE);
	}
	
	// zero out the socked address struct
	// idk if this is really needed or if we could just set the properties
	// but everyone else was doing it
	bzero((char*) &serverAddress, sizeof(serverAddress));
	
	// set to ipv6 (ipv4 connections will be accepted via maped ipv6 addresses)
	serverAddress.sin6_family = AF_INET6;
	
	// enable ipv4 connections
	int optionValue = 0; // I don't know a way to avoid this temporary variable
	setsockopt(serverHandle, IPPROTO_IPV6, IPV6_V6ONLY, (void *)&optionValue, sizeof(optionValue)); 
	
	// bind to port from argument, all addresses
	serverAddress.sin6_addr = in6addr_any;
	serverAddress.sin6_port = htons(port);
	int error = bind(serverHandle, (struct sockaddr *) &serverAddress, sizeof(serverAddress));
	if(error != 0) {
		fprintf(stderr, "Failed to bind to port %u on all addresses\n", port);
		exit(EXIT_FAILURE);
	}
	
	// set max queue length to 1 and start listening
	listen(serverHandle, 1);
	
	return serverHandle;
}

// returns connection handle or negitive on failure
int acceptConnection(int serverHandle, char greeting[]) {
	// create struct so we can receve info about the client's ip
	struct sockaddr_in6 clientAddress;
	
	// this is populated with max size and decreses to used size after call to accept
	socklen_t clientAddressMemSize = sizeof(clientAddress);
	
	// accept the connection
	int connectionHandle = accept(serverHandle, (struct sockaddr *) &clientAddress, &clientAddressMemSize);
	if (connectionHandle < 0) {
		fprintf(stderr, "Failed while trying to accept connection\n");
		// we don't want to die just because 1 client failed to connect
		return -1;
	}
	
	// print the address of the client
	// aparently ipv4 maped ipv6 addresses can be 45 chars long in some representations
	char clientAddressString[46];
	inet_ntop(AF_INET6, &(clientAddress.sin6_addr), clientAddressString, 46);
	fprintf(stderr, "New connection from %s\n", clientAddressString);
	
	// send greeting (does not include terminating \0)
	// I think send() will block untill all data is processed?
	size_t dataSize = strlen(greeting);
	size_t sentSize = send(connectionHandle, greeting, dataSize, 0);
	if(sentSize != dataSize) {
		fprintf(stderr, "Failed to send greeting\n");
		return -1;
	}
	
	// stop all sending of data (receve only now)
	//
	int error = shutdown(connectionHandle, SHUT_WR);
	if(error) {
		fprintf(stderr, "Failed to end greeting\n");
		return -1;
	}
	
	return connectionHandle;
}

// returns 0 on success, negitive on fail (-2 if connection is out of sync)
// fills BUFFER with SIZE amount of data
int readFromConnection(int connectionHandle, uint8_t* buffer, size_t size, unsigned int maxAttempts = 3) {
	for(unsigned int attempt = 0; attempt < maxAttempts; attempt++) {
		ssize_t recevedSize = recv(connectionHandle, (void*)buffer, size, MSG_WAITALL);
		// connection was closed (probably remotely)
		if(recevedSize == 0) return -1;
		
		// error reading incoming data
		if(recevedSize == -1) {
			fprintf(stderr, "Failed attempt to reed from connection (%u/%u)\n", attempt+1, maxAttempts);
			continue;
		}
		
		if((size_t)recevedSize == size) return 0; // success
		
		// if we made it here we receved a non-standard amount of data
		// the connection will be out of sync now
		fprintf(stderr, "Non-expected ammount of data (got:%ld, expected:%ld).\nThis connection is probably broken now!\n", recevedSize, size);
		return -2;
	}
	return -1;
}

// for use as an array of pixels, each containing 16-bits of info
uint16_t* frontBuffer;
uint16_t* backBuffer;
size_t screenMemSize;

int fbHandle;
struct fb_fix_screeninfo fixedFbInfo;
struct fb_var_screeninfo variableFbInfo;

typedef struct {
	uint8_t red;
	uint8_t blue;
	uint8_t green;
} pixel;

void fbInit(char* fbName) {
	// open the framebuffer, die with a usefull error if we cant open the file
	// TODO: we do not check if this is accuially a FB device
	int fbHandle = open(fbName, O_RDWR);
	if(fbHandle == -1) {
		fprintf(stderr, "Failed to open file: %s\n", fbName);
		exit(EXIT_FAILURE);
	}
	
	// get variable info, so we don't change anything accidentily when we write this struct back in a second
	ioctl(fbHandle, FBIOGET_VSCREENINFO, &variableFbInfo);
	
	// set color mote to 16-bit color
	// this makes our lives easier because we have a 16-bit datatype
	variableFbInfo.grayscale=0;
	variableFbInfo.bits_per_pixel=16;
	// push new settings to device
	int error = ioctl(fbHandle, FBIOPUT_VSCREENINFO, &variableFbInfo);
	if(error) {
		fprintf(stderr, "Error while setting color mode to 16-bits per pixel\n");
		exit(EXIT_FAILURE);
	}
	
	// get info about the video-card and display
	// I do this second because I'm not sure if line_length can change from setting color-mode
	ioctl(fbHandle, FBIOGET_FSCREENINFO, &fixedFbInfo);
	
	// calculate the size of memory for 1 screen
	screenMemSize = variableFbInfo.yres * fixedFbInfo.line_length;
	
	// mmap framebuffer
	frontBuffer = (uint16_t*)mmap(0, screenMemSize, PROT_READ | PROT_WRITE, MAP_SHARED, fbHandle, (off_t)0);
	if(frontBuffer == MAP_FAILED) {
		fprintf(stderr, "Failed to mmap framebuffer\n");
		exit(EXIT_FAILURE);
	}
	
	// get a block of memory for the back buffer
	// this is a normal malloc call bacause they don't really flip
	// we just memcpy the back to the front to avoid network delays
	backBuffer = (uint16_t*)malloc(screenMemSize);
	if(frontBuffer == NULL) {
		fprintf(stderr, "Failed to allocate 2nd buffer in ram\n");
		exit(EXIT_FAILURE);
	}
}

uint16_t rgbPixel(uint8_t r, uint8_t g, uint8_t b) {
	r /= 8; // TODO intelegently figure out length of signifigent pixels
	b /= 8;
	g /= 8;
	return (r << variableFbInfo.red.offset) |
	       (g << variableFbInfo.green.offset) |
	       (b << variableFbInfo.blue.offset);
}

void flipBuffers(void) {
	// we used to have atomic flips, but now we just memcpy
	memcpy(frontBuffer, backBuffer, screenMemSize);
}

void drawImage(pixel pixels[], bool flipWhenDone = false) {
	// loop order optimises memory access
	for (unsigned int y=0; y<variableFbInfo.yres; y++) {
		for (unsigned int x=0; x<variableFbInfo.xres; x++) {
			// calculate pixel number in aray
			unsigned long pixelNumber = (y * variableFbInfo.xres) + x;
			
			//fprintf(stderr, "(%u,%u) %lu\n", x, y, pixelNumber);
			
			// draw the pixel on to the back buffer
			pixel& p = pixels[pixelNumber];
			backBuffer[pixelNumber] = rgbPixel(p.red, p.blue, p.green);
		}
	}
	
	if(flipWhenDone) {
		flipBuffers();
	}
}

// only exposed for quick tests
typedef struct {
	unsigned int x;
	unsigned int y;
} xyPair;
xyPair getFbSize(void) {
	xyPair ret;
	ret.x = variableFbInfo.xres;
	ret.y = variableFbInfo.yres;
	return ret;
}

int main(int numArgs, char* args[]) {
	// TODO better argument handeling
	if(numArgs != 3) {
		fprintf(stderr, "Useage: %s [Frame-Buffer Device] [server port]\n", args[0]);
		exit(EXIT_FAILURE);
	}
	char* fbName = args[1];
	int port = atoi(args[2]);
	
	// set up frame buffer
	fbInit(fbName);
	
	// set up the server
	int serverHandle = serverInit(port);
	
	// we need the screen size so we can inform the client of it
	xyPair screenSize = getFbSize();
	
	// make greeting (version info and screen size)
	char greeting[100];
	// [proticall version]:[server string]
	sprintf(greeting, "1.0:fbbasic-0.1\n%ux%u\n", screenSize.x, screenSize.y);
	
	// buffer to receve network data
	size_t imageMemSize = sizeof(pixel) * screenSize.x * screenSize.y;
	pixel* image = (pixel*)malloc(imageMemSize);
	
	// loop, accepting connections untill we are killed
	for(;;) {
		// await a connection
		int connectionHandle = acceptConnection(serverHandle, greeting);
		// in the event of an error, try again
		// we don't neet to print an error, the function already did
		if(connectionHandle < 0) continue;
		
		// loop, drawing frames untill disconnect or error
		for(;;) {
			// clever cast here will fill the fields of the pixel structs sequentially
			int error = readFromConnection(connectionHandle, (uint8_t*)image, imageMemSize);
			if(error) break;
			drawImage(image, true);
		}
		
		// close old connection
		fprintf(stderr, "Closeing Connection\n");
		close(connectionHandle);
	}
}

/* TODO
 * impliment a system that updates 1 pixel at a time in a seemingly random order
 * such that 1 iteration will hit each pixel once, but not in an obvious order.
 * each time a pixel it 'hit' move R G and B 1 unit twards the destination color
 * repete this process 255 times to create a fade effect that also solves the 
 * issue of not being double buffered */
