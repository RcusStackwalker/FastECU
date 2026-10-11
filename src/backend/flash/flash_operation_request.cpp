#include "src/backend/flash/flash_operation_request.h"

namespace fastecu::flash
{

FlashOperation FlashOperationFromCommand(std::string_view command)
{
    if (command == "write")
    {
        return FlashOperation::kWrite;
    }
    if (command == "test_write")
    {
        return FlashOperation::kTestWrite;
    }
    return FlashOperation::kRead;
}

bool IsDensoTcuProtocol(std::string_view protocol)
{
    return protocol == "sub_tcu_denso_sh7055_can" || protocol == "sub_tcu_denso_sh7058_can";
}

std::string KernelPath(std::string_view dir, std::string_view file)
{
    std::string path{dir};
    if (!path.empty() && path.back() != '/')
    {
        path.push_back('/');
    }
    path.append(file);
    return path;
}

std::string ReadImageFilename(std::string_view rom_id, std::string_view timestamp)
{
    std::string name = rom_id.empty() ? std::string{"read_image_"} : std::string{rom_id};
    name.append(timestamp);
    name.append(".bin");
    return name;
}

} // namespace fastecu::flash
