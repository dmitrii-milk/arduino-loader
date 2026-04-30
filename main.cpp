#include <iostream>
#include <vector>
#include <string>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
using namespace std;

class UsbDevice
{
public:
    string portPath;
    string name;
};

int main()
{
    const CFMutableDictionaryRef match = IOServiceMatching("IOSerialBSDClient");
    io_iterator_t iterator;
    const kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault, match, &iterator);

    auto getString = [](const io_object_t dev, const CFStringRef key) -> std::string
    {
        const auto ref = static_cast<CFStringRef>(IORegistryEntryCreateCFProperty(dev, key, kCFAllocatorDefault, 0));
        if (!ref)
            return "(unknown)";
        char buf[256];
        CFStringGetCString(ref, buf, sizeof(buf), kCFStringEncodingUTF8);
        CFRelease(ref);
        return buf;
    };

    auto getInt = [](io_object_t dev, CFStringRef key) -> int
    {
        CFNumberRef ref = (CFNumberRef)IORegistryEntryCreateCFProperty(dev, key, kCFAllocatorDefault, 0);
        if (!ref)
            return -1;
        int val = -1;
        CFNumberGetValue(ref, kCFNumberIntType, &val);
        CFRelease(ref);
        return val;
    };

    vector<UsbDevice> devices;

    io_object_t device;
    while ((device = IOIteratorNext(iterator)))
    {
        std::string product = getString(device, CFSTR("kUSBProductString"));
        std::string vendor = getString(device, CFSTR("kUSBVendorString"));
        std::string callout = getString(device, CFSTR("IOCalloutDevice"));
        std::string dialin = getString(device, CFSTR("IODialinDevice"));

        UsbDevice usbDevice;
        usbDevice.portPath = callout;
        usbDevice.name = getString(device, CFSTR("IOServicePortPath"));
        devices.push_back(usbDevice);

        IOObjectRelease(device);
    }

    IOObjectRelease(iterator);

    if (kr != kIOReturnSuccess)
    {
        std::cerr << "Failed to get services: " << kr << std::endl;
        return -1;
    }

    for (int i = 0; i < devices.size(); i++)
    {
        std::cout << i + 1 << ". " << devices[i].name << std::endl;
    }

    int choice;
    std::cout << "Select a device to upload (0 to exit): ";
    std::cin >> choice;

    if (std::cin.fail())
    {
        std::cout << "Invalid input. Exiting." << std::endl;
        return 0;
    }

    if (choice < 1 || choice > devices.size())
    {
        std::cout << "Exiting." << std::endl;
        return 0;
    }

    UsbDevice selectedDevice = devices[choice - 1];
    std::cout << "Selected " << selectedDevice.name << selectedDevice.portPath << std::endl;

    string pathToFile;
    std::cout << "Input path to file: ";
    std::cin >> pathToFile;

    std::cout << pathToFile << std::endl;
    return 0;
}
