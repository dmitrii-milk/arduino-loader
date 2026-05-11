#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <cstring>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <fcntl.h>      // open()
#include <termios.h>    // termios, tcgetattr...
#include <unistd.h>     // read(), write(), close()
#include <sys/ioctl.h>  // ioctl() for DTR later

using namespace std;

#define STK_GET_SYNC  0x30
#define CRC_EOP       0x20
#define STK_INSYNC    0x14
#define STK_OK        0x10
#define ENTER_PROGMODE  0x50
#define SET_DEVICE  0x42
#define LEAVE_PROGMODE  0x51


struct HexRecord {
    uint8_t  length;
    uint16_t address;
    uint8_t  type;
    uint8_t  data[16];
};

uint8_t parseByte(const std::string& line, int pos) {
    return std::stoi(line.substr(pos, 2), nullptr, 16);
}

std::vector<HexRecord> parseHex(const std::string& pathToFile) {
    std::vector<HexRecord> records;

    std::ifstream file(pathToFile);
    if (!file.is_open()) {
        std::cerr << "Failed to open hex file" << std::endl;
        return records;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] != ':') continue;

        HexRecord record;
        record.length  = parseByte(line, 1);
        record.address = (parseByte(line, 3) << 8) | parseByte(line, 5);
        record.type    = parseByte(line, 7);

        for (int i = 0; i < record.length; i++) {
            record.data[i] = parseByte(line, 9 + i * 2);
        }

        if (record.type == 0x01) break; // end of file record

        uint8_t checksum = parseByte(line, 9 + record.length * 2);

        uint8_t sum = record.length;
        sum += (record.address >> 8) & 0xFF;
        sum += record.address & 0xFF;
        sum += record.type;
        for (int i = 0; i < record.length; i++) {
            sum += record.data[i];
        }

        if (((256 - sum) & 0xFF) != checksum) {
            std::cerr << "Checksum mismatch at address " << record.address << std::endl;
            return {};
        }

        records.push_back(record);
    }

    return records;
}

class UsbDevice {
public:
    string portPath;
    string name;
};

bool sendSyncCommand(int fileDescriptor) {
    // send bytes
    uint8_t getSyncCmd[] = {STK_GET_SYNC, CRC_EOP};

    // sync loop
    std::cout << "Start sync" << std::endl;

    bool synced = false;

    for (int i = 0; i < 5; i++) {
        write(fileDescriptor, getSyncCmd, sizeof(getSyncCmd));

        // receive bytes
        uint8_t buf[2];
        ssize_t n = read(fileDescriptor, buf, sizeof(buf));

        if (n == 2 && buf[0] == STK_INSYNC && buf[1] == STK_OK) {
            synced = true;
            break;
        }

        usleep(100000);
    }

    return synced;
}

bool setDeviceCommand(int fileDescriptor) {
    uint8_t setDeviceCmd[] = {
        SET_DEVICE, // STK_SET_DEVICE command
        0x86, // devicecode (atmega328p = 0x86)
        0x00, // revision
        0x00, // progtype
        0x01, // parmode
        0x01, // polling
        0x01, // selftimed
        0x01, // lockbytes
        0x06, // fusebytes
        0x00, // flashpollval1
        0x00, // flashpollval2
        0x00, // eeprompollval1
        0x00, // eeprompollval2
        0x00, // pagesizehigh
        0x80, // pagesizelow  (128 bytes per page)
        0x00, // eepromsizehigh
        0xFF, // eepromsizelow (256 bytes EEPROM)
        0x00, // flashsize4    (32KB flash = 0x00 0x00 0x7D 0x00)
        0x00, // flashsize3
        0x7D, // flashsize2
        0x00, // flashsize1
        CRC_EOP // sync byte — every STK500 command ends with this
    };

    write(fileDescriptor, setDeviceCmd, sizeof(setDeviceCmd));
    usleep(100000); // wait 100ms
    uint8_t resp[10]; // larger buffer
    ssize_t n = read(fileDescriptor, resp, sizeof(resp));


    for (int i = 0; i < n; i++) {
        std::cout << "resp[" << i << "] = " << (int) resp[i] << std::endl;
    }

    if (resp[0] == STK_INSYNC && resp[1] == STK_OK) {
        return true;
    }

    return false;
}

bool enterProgModeCommand(int fileDescriptor) {
    uint8_t enterProgrammingModeCmd[] = {ENTER_PROGMODE, CRC_EOP};

    write(fileDescriptor, enterProgrammingModeCmd, sizeof(enterProgrammingModeCmd));
    usleep(100000); // wait 100ms
    uint8_t respProgMode[10]; // larger buffer
    ssize_t n = read(fileDescriptor, respProgMode, sizeof(respProgMode));
    for (int i = 0; i < n; i++) {
        std::cout << "resp[" << i << "] = " << (int) respProgMode[i] << std::endl;
    }

    if (respProgMode[0] == STK_INSYNC && respProgMode[1] == STK_OK) {
        return true;
    }

    return false;
}

bool loadAddress(int fileDescriptor, uint16_t address) {
    uint16_t wordAddr = address / 2;
    uint8_t cmd[] = {0x55, (uint8_t)(wordAddr >> 8), (uint8_t)(wordAddr & 0xFF), CRC_EOP};

    write(fileDescriptor, cmd, sizeof(cmd));
    usleep(50000);

    uint8_t resp[2];
    ssize_t n = read(fileDescriptor, resp, sizeof(resp));

    return n == 2 && resp[0] == STK_INSYNC && resp[1] == STK_OK;
}

bool progPage(int fileDescriptor, uint8_t* data, uint8_t length) {
    uint8_t cmd[5 + length];
    cmd[0] = 0x64;           // STK_PROG_PAGE
    cmd[1] = 0x00;           // size high byte
    cmd[2] = length;         // size low byte
    cmd[3] = 0x46;           // 'F' = flash memory type
    memcpy(&cmd[4], data, length);
    cmd[4 + length] = CRC_EOP;

    write(fileDescriptor, cmd, sizeof(cmd));
    usleep(100000);

    uint8_t resp[2];
    ssize_t n = read(fileDescriptor, resp, sizeof(resp));

    return n == 2 && resp[0] == STK_INSYNC && resp[1] == STK_OK;
}

bool leaveProgModeCommand(int fileDescriptor) {
    uint8_t cmd[] = {LEAVE_PROGMODE, CRC_EOP};

    write(fileDescriptor, cmd, sizeof(cmd));
    usleep(100000); // wait 100ms
    uint8_t res[10]; // larger buffer
    ssize_t n = read(fileDescriptor, res, sizeof(res));
    for (int i = 0; i < n; i++) {
        std::cout << "resp[" << i << "] = " << (int) res[i] << std::endl;
    }

    if (res[0] == STK_INSYNC && res[1] == STK_OK) {
        return true;
    }

    return false;
}

int firmwareLoader(string portPath, string pathToFile) {
    std::cout << "Firmware Loader" << std::endl;

    // open file descriptor;
    int fileDescriptor = open(portPath.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);

    // if file descriptor busy return error;
    if (fileDescriptor < 0) {
        perror("open");
        return -1;
    }

    // delete O_NONBLOCK form flags... but why ?
    int flags = fcntl(fileDescriptor, F_GETFL, 0);
    fcntl(fileDescriptor, F_SETFL, flags & ~O_NONBLOCK);

    // init a struct that holds all serial port settings
    termios tty{};

    // read current settings
    tcgetattr(fileDescriptor, &tty);

    // These settings configure the serial port to raw binary mode
    cfsetspeed(&tty, B115200); // set baud rate

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

    tcsetattr(fileDescriptor, TCSANOW, &tty);

    ioctl(fileDescriptor, TIOCMBIS, TIOCM_DTR); // DTR on
    usleep(250000);
    ioctl(fileDescriptor, TIOCMBIC, TIOCM_DTR); // DTR off
    usleep(50000);

    bool synced = sendSyncCommand(fileDescriptor);

    if (!synced) {
        std::cerr << "Sync failed" << std::endl;
        close(fileDescriptor);
        return -1;
    }

    std::cout << "Synced successfully — break out of loop   " << std::endl;

    tcflush(fileDescriptor, TCIOFLUSH);

    bool setDeviceSuccessful = setDeviceCommand(fileDescriptor);

    if (setDeviceSuccessful) {
        std::cout << "SET_DEVICE success" << std::endl;
    } else {
        std::cerr << "SET_DEVICE failed" << std::endl;
        close(fileDescriptor);
        return -1;
    }

    bool enterProgModeCommandSuccessful = enterProgModeCommand(fileDescriptor);

    if (enterProgModeCommandSuccessful) {
        std::cout << "ENTER_PROGMODE success" << std::endl;
    } else {
        std::cerr << "ENTER_PROGMODE failed" << std::endl;
        close(fileDescriptor);
        return -1;
    }

    std::vector<HexRecord> records = parseHex(pathToFile);
    if (records.empty()) {
        std::cerr << "Failed to parse hex file" << std::endl;
        close(fileDescriptor);
        return -1;
    }
    std::cout << "Parsed " << records.size() << " records" << std::endl;

    const int PAGE_SIZE = 128;
    uint8_t pageBuf[PAGE_SIZE];
    memset(pageBuf, 0xFF, PAGE_SIZE);

    uint16_t currentPageAddr = records[0].address & ~(PAGE_SIZE - 1);

    for (auto& record : records) {
        uint16_t pageAddr = record.address & ~(PAGE_SIZE - 1);

        if (pageAddr != currentPageAddr) {
            if (!loadAddress(fileDescriptor, currentPageAddr)) {
                std::cerr << "LOAD_ADDRESS failed at " << currentPageAddr << std::endl;
                close(fileDescriptor);
                return -1;
            }
            if (!progPage(fileDescriptor, pageBuf, PAGE_SIZE)) {
                std::cerr << "PROG_PAGE failed at " << currentPageAddr << std::endl;
                close(fileDescriptor);
                return -1;
            }
            memset(pageBuf, 0xFF, PAGE_SIZE);
            currentPageAddr = pageAddr;
        }

        int offset = record.address - currentPageAddr;
        memcpy(pageBuf + offset, record.data, record.length);
    }

    // flush last page
    if (!loadAddress(fileDescriptor, currentPageAddr)) {
        std::cerr << "LOAD_ADDRESS failed at " << currentPageAddr << std::endl;
        close(fileDescriptor);
        return -1;
    }
    if (!progPage(fileDescriptor, pageBuf, PAGE_SIZE)) {
        std::cerr << "PROG_PAGE failed at " << currentPageAddr << std::endl;
        close(fileDescriptor);
        return -1;
    }

    std::cout << "Flash written" << std::endl;

    bool leaveProgModeCommandSuccessful = leaveProgModeCommand(fileDescriptor);

    if (leaveProgModeCommandSuccessful) {
        std::cout << "LEAVE_PROGMODE success" << std::endl;
    } else {
        std::cerr << "LEAVE_PROGMODE failed" << std::endl;
        close(fileDescriptor);
        return -1;
    }

    std::cout << "Firmware Loaded" << std::endl;
    close(fileDescriptor);
    return 0;
}

string getString(const io_object_t dev, const CFStringRef key) {
    const auto ref = static_cast<CFStringRef>(IORegistryEntryCreateCFProperty(dev, key, kCFAllocatorDefault, 0));
    if (!ref)
        return "(unknown)";
    char buf[256];
    CFStringGetCString(ref, buf, sizeof(buf), kCFStringEncodingUTF8);
    CFRelease(ref);
    return buf;
};

int main() {
    const CFMutableDictionaryRef match = IOServiceMatching("IOSerialBSDClient");
    io_iterator_t iterator;
    const kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault, match, &iterator);


    vector<UsbDevice> devices;

    io_object_t device;

    while ((device = IOIteratorNext(iterator))) {
        std::string product = getString(device, CFSTR("kUSBProductString"));
        std::string vendor = getString(device, CFSTR("kUSBVendorString"));
        std::string callout = getString(device, CFSTR("IOCalloutDevice"));

        UsbDevice usbDevice;
        usbDevice.portPath = callout;
        usbDevice.name = callout;
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
