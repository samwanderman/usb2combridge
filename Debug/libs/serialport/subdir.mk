################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
CPP_SRCS += \
../libs/serialport/SerialPort.cpp 

OBJS += \
./libs/serialport/SerialPort.o 

CPP_DEPS += \
./libs/serialport/SerialPort.d 


# Each subdirectory must supply rules for building sources it contributes
libs/serialport/%.o: ../libs/serialport/%.cpp
	@echo 'Building file: $<'
	@echo 'Invoking: GCC C++ Compiler'
	g++ -std=c++1y -O0 -g3 -Wall -c -fmessage-length=0 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


