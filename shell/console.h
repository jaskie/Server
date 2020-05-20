#pragma once
#include <fstream>
#include <streambuf>

class console
{
private:
    std::wstreambuf* cin_buffer, * cout_buffer, * cerr_buffer;
    std::wfstream console_input, console_output, console_error;
    const int allocated_;
public:
    console();
    ~console();
};
