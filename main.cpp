#include <iostream>
#include <vector>
#include <string>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <fcntl.h>      // open()
#include <termios.h>    // termios, tcgetattr...
#include <unistd.h>     // read(), write(), close()
#include <sys/ioctl.h>  // ioctl() for DTR later

using namespace std;

class UsbDevice {
public:
    string portPath;
    string name;
};

int firmwareLoader(string portPath, string pathToFile) {
    std::cout << "Firmware Loader" << std::endl;

    int fd = open(portPath.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);

    if (fd < 0) {
        perror("open");
        return -1;
    }

    struct termios tty;
    tcgetattr(fd, &tty);

    cfsetspeed(&tty, B115200); // baud rate

    tty.c_cflag &= ~PARENB; // no parity
    tty.c_cflag &= ~CSTOPB; // 1 stop bit
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8; // 8 data bits

    tty.c_cflag &= ~CRTSCTS; // no hardware flow control
    tty.c_cflag |= CREAD | CLOCAL; // enable receiver

    tty.c_lflag &= ~ICANON; // raw mode (not line-by-line)
    tty.c_lflag &= ~ECHO; // no echo
    tty.c_lflag &= ~ISIG; // no signal chars (Ctrl+C etc)

    tty.c_iflag &= ~(IXON | IXOFF | IXANY); // no software flow control
    tty.c_oflag &= ~OPOST; // raw output

    tcsetattr(fd, TCSANOW, &tty);

    // send bytes
    uint8_t cmd[] = {0x30, 0x20};
    write(fd, cmd, sizeof(cmd));

    // receive bytes
    uint8_t buf[2];
    read(fd, buf, sizeof(buf));

    std::cout << "Firmware Loaded" << std::endl;

    return 0;
}

int main() {
    const CFMutableDictionaryRef match = IOServiceMatching("IOSerialBSDClient");
    io_iterator_t iterator;
    const kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault, match, &iterator);

    auto getString = [](const io_object_t dev, const CFStringRef key) -> std::string {
        const auto ref = static_cast<CFStringRef>(IORegistryEntryCreateCFProperty(dev, key, kCFAllocatorDefault, 0));
        if (!ref)
            return "(unknown)";
        char buf[256];
        CFStringGetCString(ref, buf, sizeof(buf), kCFStringEncodingUTF8);
        CFRelease(ref);
        return buf;
    };

    vector<UsbDevice> devices;

    io_object_t device;
    while ((device = IOIteratorNext(iterator))) {
        std::string product = getString(device, CFSTR("kUSBProductString"));
        std::string vendor = getString(device, CFSTR("kUSBVendorString"));
        std::string callout = getString(device, CFSTR("IOCalloutDevice"));

        UsbDevice usbDevice;
        usbDevice.portPath = callout;
        usbDevice.name = vendor + " / " + product;
        devices.push_back(usbDevice);

        IOObjectRelease(device);
    }

    IOObjectRelease(iterator);

    if (kr != kIOReturnSuccess) {
        std::cerr << "Failed to get services: " << kr << std::endl;
        return -1;
    }

    for (int i = 0; i < devices.size(); i++) {
        std::cout << i + 1 << ". " << devices[i].name << std::endl;
    }

    int choice;
    std::cout << "Select a device to upload (0 to exit): ";
    std::cin >> choice;

    if (std::cin.fail()) {
        std::cout << "Invalid input. Exiting." << std::endl;
        return 0;
    }

    if (choice < 1 || choice > devices.size()) {
        std::cout << "Exiting." << std::endl;
        return 0;
    }

    UsbDevice selectedDevice = devices[choice - 1];
    std::cout << "Selected " << selectedDevice.name << selectedDevice.portPath << std::endl;

    string pathToFile;
    std::cout << "Input path to file: ";
    std::cin >> pathToFile;
    std::cout << pathToFile << std::endl;

    const int result = firmwareLoader(selectedDevice.portPath, pathToFile);

    if (result == -1) {
        return result;
    }

    return 0;
}
