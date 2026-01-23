#include "VCU_Decode.h"
#include <fstream>
#include <iostream>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <video_file.mp4>" << std::endl;
        return 1;
    }
    std::string const filename = argv[1];

    gst_init(nullptr, nullptr);

    VCU_Decode decoder;
    decoder.create_pipeline();
    decoder.configure_elements(filename);
    decoder.configure_app_sink();

    decoder.start_pipeline(); // blocks until EOS

    std::cout << "End" << std::endl;

    return 0;
}

