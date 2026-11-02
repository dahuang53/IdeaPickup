#!/usr/bin/env python3
"""Send provider credentials over the dedicated non-echoing USB command.
Secrets are never command-line arguments, files, or diagnostic output.
"""
import argparse
import getpass
import json
import sys
import time
import warnings


def exchange(port, payload, timeout=18):
    data=json.dumps(payload,ensure_ascii=True,separators=(',',':')).encode('utf-8')
    if len(data)>1024:
        raise RuntimeError('输入超出1024字节限制，未发送凭据')
    port.write(b'cloud_config\n')
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        line=port.readline()
        if line.strip()==b'CLOUD_CONFIG BUSY':
            raise RuntimeError('设备忙碌；请保持配置热点开启、停止录放后重试')
        if line.strip()==b'CLOUD_CONFIG READY':
            break
    else:
        raise RuntimeError('设备未进入配置输入，未发送凭据')
    port.write(data+b'\n')
    while time.monotonic()<deadline:
        line=port.readline().strip()
        if line.startswith(b'CLOUD_CONFIG RESULT '):
            if line==b'CLOUD_CONFIG RESULT 0':
                return
            raise RuntimeError('设备未确认保存，请在网页核对配置；输入不会自动重发')
    raise RuntimeError('保存结果未确认，请在网页核对；输入不会自动重发')


def hidden(prompt):
    with warnings.catch_warnings():
        warnings.simplefilter("error",getpass.GetPassWarning)
        return getpass.getpass(prompt)

def open_port(factory, name):
    # Existing application CDC requires DTR for connected/READY output.
    # Pre-set controls; no reset sequence, RTS pulse or bootloader command.
    port=factory(port=None,baudrate=115200,timeout=0.25,write_timeout=2,exclusive=True)
    port.dtr=True;port.rts=False;port.port=name;port.open()
    return port

def main():
    parser=argparse.ArgumentParser(description='IdeaPickup USB 云服务配置；设备须空闲且配置热点开启')
    parser.add_argument('--port',help='串口；未指定时选择唯一Espressif设备')
    parser.add_argument('--clear-qwen',action='store_true')
    parser.add_argument('--clear-siyuan',action='store_true')
    args=parser.parse_args()
    import serial
    from serial.tools import list_ports
    name=args.port
    if not name:
        ports=[p.device for p in list_ports.comports() if p.vid==0x303a]
        if len(ports)!=1:
            print('请用 --port 指定目标串口。当前Espressif设备：'+', '.join(ports),file=sys.stderr)
            return 1
        name=ports[0]
    if not sys.stdin.isatty():
        print('请在可信本机交互终端运行，拒绝回显输入。',file=sys.stderr)
        return 1
    payload={'clear_qwen':args.clear_qwen,'clear_siyuan':args.clear_siyuan}
    try:
        if not args.clear_qwen:
            payload['workspace']=input('千问Workspace ID（留空保留）：').strip()
            payload['qwen_key']=hidden('千问Key（隐藏输入，留空保留）：')
        if not args.clear_siyuan:
            payload['siyuan_token']=hidden('思源Token（隐藏输入，留空保留）：')
        port=open_port(serial.Serial,name)
        try:
            exchange(port,payload)
        finally:
            port.close()
        print('配置已保存；请回配网页查看状态。保存未调用转写或投递。')
        return 0
    except (RuntimeError,serial.SerialException,EOFError,KeyboardInterrupt,getpass.GetPassWarning):
        # Some transport exceptions contain their input or filesystem context.
        # Never print the exception or serial traffic containing user data.
        print('配置未确认；请核对串口、设备空闲和热点状态，再查看网页。',file=sys.stderr)
        return 1
    finally:
        payload.clear()  # Python strings cannot promise physical RAM zeroization.

if __name__=='__main__':
    raise SystemExit(main())
