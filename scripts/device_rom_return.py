#!/usr/bin/env python3
"""Reset the connected S3 ROM into its verified app; no flash/eFuse programming."""
from firmware_bundle import require,ROOT

def open_serial(factory,port):
 connection=factory(port=None,baudrate=115200,timeout=.1,write_timeout=2,exclusive=True)
 connection.rts=False;connection.dtr=False;connection.port=port;connection.open();return connection

def controlled_return(connection,esp,mac):
 require(not connection.rts and not connection.dtr,'ROM serial controls must remain idle')
 esp.connect(mode='no-reset',attempts=1,warnings=False)
 require(esp.CHIP_NAME=='ESP32-S3' and not esp.IS_STUB,'unsupported ROM')
 require(bytes(esp.read_mac())==mac,'ROM device changed')
 security=esp.get_security_info(cache=False)
 flags=security['parsed_flags']
 require(not flags['SECURE_BOOT_EN'] and not flags['SECURE_DOWNLOAD_ENABLE'] and security['flash_crypt_cnt'].bit_count()%2==0,'unsupported protected ROM')
 require(esp.get_uart_no()==4,'USB Serial/JTAG ROM required')
 # GPIO0 held low would force download again after the watchdog. Never claim
 # an application return from the reset request; installer verification is separate.
 require(esp.read_reg(0x6000403c)&1,'release GPIO0 before returning from ROM')
 before=esp.read_reg(esp.RTC_CNTL_OPTION1_REG)
 esp.write_reg(esp.RTC_CNTL_OPTION1_REG,0,esp.RTC_CNTL_FORCE_DOWNLOAD_BOOT_MASK)
 require(esp.read_reg(esp.RTC_CNTL_OPTION1_REG)==before&~esp.RTC_CNTL_FORCE_DOWNLOAD_BOOT_MASK,'force-download clear not confirmed')
 esp.watchdog_reset();return 'RESET_REQUESTED_BOOT_NOT_CONFIRMED'

def reset(port,mac):
 import os
 os.environ["ESPTOOL_CFGFILE"]=str(ROOT/"scripts/esptool-protected.cfg")
 import serial
 from esptool.targets.esp32s3 import ESP32S3ROM
 class ROM(ESP32S3ROM):
  def _post_connect(self):pass
 connection=open_serial(serial.Serial,port)
 try:return controlled_return(connection,ROM(port=connection,baud=115200,trace_enabled=False),mac)
 finally:connection.close()
