#!/usr/bin/env python3
"""
SmartlinkTechnology M01 Device Driver

A Python driver for communicating with the SmartlinkTechnology M01 device.
This driver provides a high-level interface for device initialization,
configuration, data transmission, and status monitoring.

Author: Driver Generator
License: MIT
"""

import serial
import time
import logging
from typing import Optional, Dict, Any, List, Callable
from enum import Enum
from dataclasses import dataclass
import threading
from contextlib import contextmanager

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)


class DeviceStatus(Enum):
    """Enumeration of possible device status states."""
    DISCONNECTED = "disconnected"
    CONNECTED = "connected"
    INITIALIZING = "initializing"
    READY = "ready"
    ERROR = "error"
    BUSY = "busy"


class ErrorCode(Enum):
    """Enumeration of error codes."""
    NONE = 0
    CONNECTION_FAILED = 1
    TIMEOUT = 2
    INVALID_RESPONSE = 3
    DEVICE_ERROR = 4
    CONFIGURATION_ERROR = 5
    COMMUNICATION_ERROR = 6


@dataclass
class DeviceInfo:
    """Data class containing device information."""
    manufacturer: str
    model: str
    firmware_version: str
    serial_number: str
    status: DeviceStatus


@dataclass
class CommunicationConfig:
    """Configuration for serial communication."""
    baudrate: int = 115200
    bytesize: int = serial.EIGHTBITS
    parity: str = serial.PARITY_NONE
    stopbits: float = serial.STOPBITS_ONE
    timeout: float = 1.0
    write_timeout: float = 1.0


class SmartlinkM01Driver:
    """
    Main driver class for SmartlinkTechnology M01 device.
    
    This class provides methods to:
    - Initialize and configure the device
    - Send and receive data
    - Monitor device status
    - Handle errors and reconnection
    """
    
    # Command constants
    CMD_INIT = b"AT+INIT\r\n"
    CMD_STATUS = b"AT+STATUS\r\n"
    CMD_RESET = b"AT+RESET\r\n"
    CMD_VERSION = b"AT+VERSION\r\n"
    CMD_CONFIG = b"AT+CONFIG="
    CMD_SEND = b"AT+SEND="
    CMD_READ = b"AT+READ\r\n"
    
    def __init__(self, port: str = "/dev/ttyUSB0", config: Optional[CommunicationConfig] = None):
        """
        Initialize the M01 driver.
        
        Args:
            port: Serial port path (e.g., '/dev/ttyUSB0' or 'COM3')
            config: Communication configuration object
        """
        self.port = port
        self.config = config or CommunicationConfig()
        self.serial_port: Optional[serial.Serial] = None
        self._status = DeviceStatus.DISCONNECTED
        self._device_info: Optional[DeviceInfo] = None
        self._lock = threading.Lock()
        self._event_callbacks: Dict[str, List[Callable]] = {}
        
        logger.info(f"M01 Driver initialized for port: {port}")
    
    @property
    def status(self) -> DeviceStatus:
        """Get current device status."""
        return self._status
    
    @property
    def is_connected(self) -> bool:
        """Check if device is connected and ready."""
        return self._status == DeviceStatus.READY
    
    @property
    def device_info(self) -> Optional[DeviceInfo]:
        """Get device information."""
        return self._device_info
    
    def connect(self) -> bool:
        """
        Establish connection to the device.
        
        Returns:
            bool: True if connection successful, False otherwise
        """
        try:
            logger.info(f"Attempting to connect to {self.port}...")
            self._status = DeviceStatus.INITIALIZING
            
            self.serial_port = serial.Serial(
                port=self.port,
                baudrate=self.config.baudrate,
                bytesize=self.config.bytesize,
                parity=self.config.parity,
                stopbits=self.config.stopbits,
                timeout=self.config.timeout,
                write_timeout=self.config.write_timeout
            )
            
            # Allow time for device to stabilize
            time.sleep(0.5)
            
            # Verify connection
            if self._verify_connection():
                self._status = DeviceStatus.CONNECTED
                logger.info("Successfully connected to M01 device")
                return True
            else:
                logger.error("Connection verification failed")
                self._status = DeviceStatus.ERROR
                return False
                
        except serial.SerialException as e:
            logger.error(f"Serial connection error: {e}")
            self._status = DeviceStatus.ERROR
            return False
        except Exception as e:
            logger.error(f"Unexpected error during connection: {e}")
            self._status = DeviceStatus.ERROR
            return False
    
    def _verify_connection(self) -> bool:
        """
        Verify the connection by sending a test command.
        
        Returns:
            bool: True if verification successful
        """
        try:
            response = self._send_command(self.CMD_VERSION)
            return response is not None and len(response) > 0
        except Exception:
            return False
    
    def initialize(self) -> bool:
        """
        Initialize the device with default configuration.
        
        Returns:
            bool: True if initialization successful
        """
        if not self.is_connected:
            logger.error("Cannot initialize: device not connected")
            return False
        
        try:
            logger.info("Initializing M01 device...")
            
            # Send initialization command
            response = self._send_command(self.CMD_INIT)
            if not response:
                logger.error("Initialization command failed")
                return False
            
            # Retrieve device information
            self._device_info = self._get_device_info()
            
            self._status = DeviceStatus.READY
            logger.info(f"Device initialized successfully: {self._device_info}")
            return True
            
        except Exception as e:
            logger.error(f"Initialization error: {e}")
            self._status = DeviceStatus.ERROR
            return False
    
    def _get_device_info(self) -> DeviceInfo:
        """
        Query device for information.
        
        Returns:
            DeviceInfo: Object containing device details
        """
        try:
            # Get version info
            version_response = self._send_command(self.CMD_VERSION)
            firmware_version = self._parse_response(version_response) if version_response else "unknown"
            
            # Get status info
            status_response = self._send_command(self.CMD_STATUS)
            serial_number = self._parse_response(status_response) if status_response else "unknown"
            
            return DeviceInfo(
                manufacturer="SmartlinkTechnology",
                model="M01",
                firmware_version=firmware_version,
                serial_number=serial_number,
                status=self._status
            )
        except Exception as e:
            logger.warning(f"Could not retrieve complete device info: {e}")
            return DeviceInfo(
                manufacturer="SmartlinkTechnology",
                model="M01",
                firmware_version="unknown",
                serial_number="unknown",
                status=self._status
            )
    
    def _send_command(self, command: bytes, wait_time: float = 0.1) -> Optional[str]:
        """
        Send a command to the device and wait for response.
        
        Args:
            command: Command bytes to send
            wait_time: Time to wait for response
            
        Returns:
            Response string or None if failed
        """
        with self._lock:
            try:
                if not self.serial_port or not self.serial_port.is_open:
                    logger.error("Serial port not available")
                    return None
                
                # Send command
                self.serial_port.write(command)
                self.serial_port.flush()
                
                # Wait for response
                time.sleep(wait_time)
                
                # Read response
                if self.serial_port.in_waiting > 0:
                    response = self.serial_port.read(self.serial_port.in_waiting).decode('utf-8', errors='ignore')
                    logger.debug(f"Command response: {response.strip()}")
                    return response
                else:
                    logger.debug("No response received")
                    return None
                    
            except serial.SerialException as e:
                logger.error(f"Serial communication error: {e}")
                return None
            except Exception as e:
                logger.error(f"Command execution error: {e}")
                return None
    
    def _parse_response(self, response: str) -> str:
        """
        Parse raw response from device.
        
        Args:
            response: Raw response string
            
        Returns:
            Parsed response content
        """
        if not response:
            return ""
        
        # Remove common prefixes/suffixes
        lines = response.strip().split('\n')
        for line in lines:
            line = line.strip()
            if line and not line.startswith('AT') and not line.startswith('OK') and not line.startswith('ERROR'):
                return line
        return response.strip()
    
    def send_data(self, data: bytes) -> bool:
        """
        Send data through the device.
        
        Args:
            data: Data bytes to send
            
        Returns:
            bool: True if send successful
        """
        if not self.is_connected:
            logger.error("Cannot send data: device not ready")
            return False
        
        try:
            # Encode data length
            data_length = len(data)
            command = f"{self.CMD_SEND.decode()}{data_length}\r\n".encode()
            
            response = self._send_command(command)
            if response and "OK" in response:
                # Send actual data
                with self._lock:
                    self.serial_port.write(data)
                    self.serial_port.flush()
                logger.info(f"Successfully sent {data_length} bytes")
                return True
            else:
                logger.error("Send command failed")
                return False
                
        except Exception as e:
            logger.error(f"Data send error: {e}")
            return False
    
    def read_data(self, max_bytes: int = 1024) -> Optional[bytes]:
        """
        Read data from the device.
        
        Args:
            max_bytes: Maximum number of bytes to read
            
        Returns:
            Received data bytes or None
        """
        if not self.is_connected:
            logger.error("Cannot read data: device not ready")
            return None
        
        try:
            # Request data
            response = self._send_command(self.CMD_READ)
            if not response:
                return None
            
            # Read available data
            with self._lock:
                if self.serial_port.in_waiting > 0:
                    bytes_to_read = min(max_bytes, self.serial_port.in_waiting)
                    data = self.serial_port.read(bytes_to_read)
                    logger.info(f"Read {len(data)} bytes")
                    return data
                else:
                    return None
                    
        except Exception as e:
            logger.error(f"Data read error: {e}")
            return None
    
    def configure(self, settings: Dict[str, Any]) -> bool:
        """
        Configure device settings.
        
        Args:
            settings: Dictionary of configuration parameters
            
        Returns:
            bool: True if configuration successful
        """
        if not self.is_connected:
            logger.error("Cannot configure: device not ready")
            return False
        
        try:
            for key, value in settings.items():
                command = f"{self.CMD_CONFIG.decode()}{key}={value}\r\n".encode()
                response = self._send_command(command)
                
                if not response or "ERROR" in response:
                    logger.error(f"Failed to configure {key}")
                    return False
            
            logger.info("Configuration updated successfully")
            return True
            
        except Exception as e:
            logger.error(f"Configuration error: {e}")
            return False
    
    def reset(self) -> bool:
        """
        Reset the device.
        
        Returns:
            bool: True if reset successful
        """
        try:
            logger.info("Resetting device...")
            response = self._send_command(self.CMD_RESET)
            
            if response:
                # Wait for device to reboot
                time.sleep(2.0)
                logger.info("Device reset completed")
                return True
            else:
                logger.error("Reset command failed")
                return False
                
        except Exception as e:
            logger.error(f"Reset error: {e}")
            return False
    
    def get_status(self) -> DeviceStatus:
        """
        Query current device status.
        
        Returns:
            DeviceStatus: Current status enumeration
        """
        if not self.serial_port or not self.serial_port.is_open:
            self._status = DeviceStatus.DISCONNECTED
            return self._status
        
        try:
            response = self._send_command(self.CMD_STATUS)
            if response and "OK" in response:
                self._status = DeviceStatus.READY
            else:
                self._status = DeviceStatus.BUSY
        except Exception:
            self._status = DeviceStatus.ERROR
        
        return self._status
    
    def register_callback(self, event: str, callback: Callable) -> None:
        """
        Register a callback for device events.
        
        Args:
            event: Event name (e.g., 'status_change', 'data_received')
            callback: Callback function to invoke
        """
        if event not in self._event_callbacks:
            self._event_callbacks[event] = []
        self._event_callbacks[event].append(callback)
        logger.info(f"Registered callback for event: {event}")
    
    def _trigger_event(self, event: str, *args, **kwargs) -> None:
        """Trigger all callbacks for an event."""
        if event in self._event_callbacks:
            for callback in self._event_callbacks[event]:
                try:
                    callback(*args, **kwargs)
                except Exception as e:
                    logger.error(f"Callback error for {event}: {e}")
    
    @contextmanager
    def connection_context(self):
        """
        Context manager for automatic connection handling.
        
        Usage:
            with driver.connection_context():
                driver.send_data(b"hello")
        """
        connected = False
        try:
            if not self.is_connected:
                connected = self.connect()
                if connected:
                    self.initialize()
            yield self
        finally:
            if connected:
                self.disconnect()
    
    def disconnect(self) -> None:
        """Close connection to the device."""
        try:
            if self.serial_port and self.serial_port.is_open:
                self.serial_port.close()
                logger.info("Disconnected from M01 device")
        except Exception as e:
            logger.error(f"Disconnect error: {e}")
        finally:
            self._status = DeviceStatus.DISCONNECTED
            self.serial_port = None
    
    def __enter__(self):
        """Enter context manager."""
        self.connect()
        self.initialize()
        return self
    
    def __exit__(self, exc_type, exc_val, exc_tb):
        """Exit context manager."""
        self.disconnect()
    
    def __del__(self):
        """Destructor to ensure cleanup."""
        self.disconnect()


# Convenience functions
def scan_ports() -> List[str]:
    """
    Scan for available serial ports.
    
    Returns:
        List of available port names
    """
    import serial.tools.list_ports
    ports = serial.tools.list_ports.comports()
    return [port.device for port in ports]


def find_m01_device() -> Optional[str]:
    """
    Attempt to find an M01 device on available ports.
    
    Returns:
        Port name if found, None otherwise
    """
    ports = scan_ports()
    for port in ports:
        try:
            driver = SmartlinkM01Driver(port=port)
            if driver.connect():
                driver.disconnect()
                logger.info(f"Found M01 device on {port}")
                return port
        except Exception:
            continue
    return None


if __name__ == "__main__":
    # Example usage
    print("SmartlinkTechnology M01 Driver")
    print("=" * 40)
    
    # Scan for devices
    print("\nScanning for available ports...")
    available_ports = scan_ports()
    for port in available_ports:
        print(f"  - {port}")
    
    # Example connection (uncomment to use)
    # with SmartlinkM01Driver("/dev/ttyUSB0") as driver:
    #     driver.send_data(b"Hello, M01!")
    #     response = driver.read_data()
    #     print(f"Response: {response}")
