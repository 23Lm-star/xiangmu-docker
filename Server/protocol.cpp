#include "protocol.h"
#include <stdlib.h>
#include <string.h>
#include "memalloc.h"

// PDU 内存管理规则：
// 1. 所有 PDU 必须由分配该 PDU 的同一进程内配套函数释放
//    - Client: mkPDU() 用 malloc → 必须用 free() 释放
//    - Server: mkPDU() 用 memalloc → 必须用 memfree() 释放
// 2. 禁止跨进程/跨策略释放 PDU
// 3. 接收缓冲区中的原始字节不由 seedMsg() 释放
PDU *mkPDU(uint uiMsgLen)
{
    uint uitotallen = sizeof(PDU) + uiMsgLen;
    PDU* pdu = (PDU*)memalloc(uitotallen);
    if(pdu == nullptr){
        exit(1);
    }
    memset(pdu, 0, uitotallen);
    pdu->uiTotalLen = uitotallen;
    pdu->uiMsgLen = uiMsgLen;
    return pdu;
}
