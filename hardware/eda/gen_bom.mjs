import fs from 'node:fs';
import zlib from 'node:zlib';

// ==== ACCURATE component list (live-verified from EDA, 2026-08-23) ====
// Main page: C1-C5, FPC1, JOY1, K1-K5, LED1-3, Q1, R1,R2,R3,R11,R12, SCREW1-4, U1, U2
// USB page:  BT1, C6-C13, D1,D2,D3, F1, LED4,LED5, Q2, R4-R10, SW1, TP*, U3,U4,U5, USBC1
const comps = [
  // ---- Resistors ----
  { des:'R1', cat:'电阻', val:'10Ω 0402? (实为0603)', fp:'0603', lcsc:'C22859', note:'厚膜±1% 100mW' },
  { des:'R2', cat:'电阻', val:'4.7kΩ', fp:'0402', lcsc:'C25900', note:'复位 DTR→基极' },
  { des:'R3', cat:'电阻', val:'100Ω', fp:'0603', lcsc:'C22775', note:'复位 集电极→T5_RST' },
  { des:'R4', cat:'电阻', val:'5.1kΩ', fp:'0603', lcsc:'C23186', note:'Type-C CC1 下拉' },
  { des:'R5', cat:'电阻', val:'2kΩ', fp:'0603', lcsc:'C22975', note:'CH340 TXD→T5 RX 隔离' },
  { des:'R6', cat:'电阻', val:'4.7kΩ', fp:'0402', lcsc:'C25900', note:'' },
  { des:'R7', cat:'电阻', val:'4.7kΩ', fp:'0402', lcsc:'C25900', note:'背光 Q2 基极' },
  { des:'R8', cat:'电阻', val:'5.1kΩ', fp:'0603', lcsc:'C23186', note:'Type-C CC2 下拉' },
  { des:'R9', cat:'电阻', val:'100Ω', fp:'0603', lcsc:'C22775', note:'复位 集电极→T5_RST' },
  { des:'R10', cat:'电阻', val:'4.7kΩ', fp:'0402', lcsc:'C25900', note:'复位 DTR→基极' },
  { des:'R11', cat:'电阻', val:'0Ω', fp:'0402', lcsc:'C17168', note:'LCD_SCK 阻尼(预留,振铃换22Ω)' },
  { des:'R12', cat:'电阻', val:'0Ω', fp:'0402', lcsc:'C17168', note:'LCD_MOSI 阻尼(预留,振铃换22Ω)' },
  // ---- Capacitors ----
  { des:'C1', cat:'电容', val:'1uF 25V X5R', fp:'0402', lcsc:'C52923', note:'去耦' },
  { des:'C2', cat:'电容', val:'100nF 35V X7R', fp:'0402', lcsc:'C1525', note:'去耦' },
  { des:'C3', cat:'电容', val:'100nF 35V X7R', fp:'0402', lcsc:'C1525', note:'LCD 电源去耦' },
  { des:'C4', cat:'电容', val:'100nF 35V X7R', fp:'0402', lcsc:'C1525', note:'LCD 电源去耦' },
  { des:'C5', cat:'电容', val:'100nF 35V X7R', fp:'0402', lcsc:'C1525', note:'去耦' },
  { des:'C6', cat:'电容', val:'100nF 35V X7R', fp:'0402', lcsc:'C1525', note:'CH340 VCC 去耦(贴脚)' },
  { des:'C7', cat:'电容', val:'1uF 25V X5R', fp:'0402', lcsc:'C52923', note:'CH340 VCC 储能' },
  { des:'C8', cat:'电容', val:'10uF 10V X5R', fp:'0603', lcsc:'C19702', note:'去耦/储能' },
  { des:'C9', cat:'电容', val:'100nF 35V X7R', fp:'0402', lcsc:'C1525', note:'去耦(贴脚)' },
  { des:'C10', cat:'电容', val:'1uF 16V X5R', fp:'0603', lcsc:'C15849', note:'去耦' },
  { des:'C11', cat:'电容', val:'10uF 10V X5R', fp:'0603', lcsc:'C19702', note:'储能' },
  { des:'C12', cat:'电容', val:'10uF 10V X5R', fp:'0603', lcsc:'C19702', note:'LDO 输入(VBUS)' },
  { des:'C13', cat:'电容', val:'10uF 10V X5R', fp:'0603', lcsc:'C19702', note:'LDO 输出' },
  // ---- ICs ----
  { des:'U1', cat:'模组接口', val:'TUYA T5AI-Core (44P 母排)', fp:'2×1×22 母排', lcsc:'—', note:'开发板经母排接入,非贴片,不下单(母排另购)' },
  { des:'U2', cat:'IC', val:'SN74LV1T34DBVR', fp:'SOT-23-5', lcsc:'C100024', note:'电平转换 3.3→5V(WS2812)' },
  { des:'U3', cat:'IC', val:'CH340C', fp:'SOP-16', lcsc:'C84681', note:'USB-UART 桥' },
  { des:'U4', cat:'其它', val:'(占位/接口符号)', fp:'—', lcsc:'—', note:'⚠️空器件,确认是否删除/是否下单' },
  { des:'U5', cat:'IC', val:'HE9073A33M5R (LDO 3.3V)', fp:'SOT-23-5', lcsc:'C723789', note:'★更正:U5=LDO,非CH340。VBUS→3.3V 专供 CH340' },
  // ---- Transistors ----
  { des:'Q1', cat:'三极管', val:'S8050 (SS8050) NPN', fp:'SOT-23', lcsc:'C2150', note:'复位管' },
  { des:'Q2', cat:'三极管', val:'S8050 (SS8050) NPN', fp:'SOT-23', lcsc:'C2150', note:'背光低边开关' },
  // ---- Protection ----
  { des:'D1', cat:'保护', val:'PTVS5V0S1UR,115 (TVS)', fp:'SOD-123W', lcsc:'C85364', note:'⚠️符号: 数据线? Vrwm5V/钳位9.2V' },
  { des:'D2', cat:'保护', val:'双向ESD 5V/40V (KLXES15AAA1)', fp:'0402', lcsc:'C2892490', note:'USB D+/D- ESD' },
  { des:'D3', cat:'保护', val:'PTVS5V0S1UR,115 (TVS)', fp:'SOD-123W', lcsc:'C85364', note:'VBUS 过压保护' },
  { des:'F1', cat:'保护', val:'BSMD0805-050-6V (PPTC)', fp:'0805', lcsc:'C883108', note:'VBUS 自恢复保险 0.5A hold/1A trip' },
  // ---- LEDs ----
  { des:'LED1', cat:'LED', val:'WS2812B (XL-0807RGBC-2812B)', fp:'SMD-4P 2×1.8', lcsc:'C3646929', note:'RGB 状态灯' },
  { des:'LED2', cat:'LED', val:'WS2812B (XL-0807RGBC-2812B)', fp:'SMD-4P 2×1.8', lcsc:'C3646929', note:'RGB 状态灯' },
  { des:'LED3', cat:'LED', val:'WS2812B (XL-0807RGBC-2812B)', fp:'SMD-4P 2×1.8', lcsc:'C3646929', note:'RGB 状态灯' },
  { des:'LED4', cat:'LED', val:'红光 LED (XL-1608SURC-06)', fp:'0603', lcsc:'C84256', note:'电源指示(BAT_P)' },
  { des:'LED5', cat:'LED', val:'红光 LED (XL-1608SURC-06)', fp:'0603', lcsc:'C84256', note:'USB 插入指示(VBUS)' },
  // ---- Switches / connectors ----
  { des:'K1', cat:'机械键', val:'CPG151101D06 茶轴', fp:'插件', lcsc:'C49234237', note:'MX 茶轴,键距19.05mm' },
  { des:'K2', cat:'机械键', val:'CPG151101D06 茶轴', fp:'插件', lcsc:'C49234237', note:'' },
  { des:'K3', cat:'机械键', val:'CPG151101D06 茶轴', fp:'插件', lcsc:'C49234237', note:'' },
  { des:'K4', cat:'机械键', val:'CPG151101D06 茶轴', fp:'插件', lcsc:'C49234237', note:'' },
  { des:'K5', cat:'机械键', val:'CPG151101D06 茶轴', fp:'插件', lcsc:'C49234237', note:'' },
  { des:'JOY1', cat:'开关', val:'WS-1002 5向摇杆', fp:'SMD-8P 7.5×7.5', lcsc:'C42377834', note:'4向+中心' },
  { des:'SW1', cat:'开关', val:'SK12D07VG5', fp:'插件 8.7×4.4', lcsc:'C431548', note:'电池总开关 SPDT 0.5A/50V' },
  { des:'USBC1', cat:'连接器', val:'Type-C 16P 母卧贴', fp:'SMD', lcsc:'C2874672', note:'USB 口(核对库存款编号)' },
  { des:'FPC1', cat:'连接器', val:'0.5K-DX-18PWB-RW 18P', fp:'SMD 0.5mm 卧贴', lcsc:'C51901319', note:'LCD FPC 下接翻盖' },
  { des:'BT1', cat:'连接器', val:'JST PH 1.25 2P 卧贴', fp:'SMD 1.25mm', lcsc:'C964022', note:'锂电池座(核对库存款)' },
  // ---- Test points / screws (不下单) ----
  { des:'TP1', cat:'测试点', val:'测试点', fp:'—', lcsc:'—', note:'不下单' },
  { des:'TP_GND1', cat:'测试点', val:'测试点 GND', fp:'—', lcsc:'—', note:'不下单' },
  { des:'TP_BAT1', cat:'测试点', val:'测试点 BAT', fp:'—', lcsc:'—', note:'不下单' },
  { des:'TP_BAT2', cat:'测试点', val:'测试点/测试针 BAT', fp:'—', lcsc:'—', note:'不下单' },
  { des:'SCREW1', cat:'机械', val:'M2.5 安装孔', fp:'NPTH', lcsc:'—', note:'固定孔,非元件' },
  { des:'SCREW2', cat:'机械', val:'M2.5 安装孔', fp:'NPTH', lcsc:'—', note:'固定孔,非元件' },
  { des:'SCREW3', cat:'机械', val:'M2.5 安装孔', fp:'NPTH', lcsc:'—', note:'固定孔,非元件' },
  { des:'SCREW4', cat:'机械', val:'M2.5 安装孔', fp:'NPTH', lcsc:'—', note:'固定孔,非元件' },
];

// group by cat|val|fp|lcsc
const groups = {};
for (const c of comps) {
  const key = c.cat + '|' + c.val + '|' + c.fp + '|' + c.lcsc;
  if (!groups[key]) groups[key] = { cat:c.cat, val:c.val, fp:c.fp, lcsc:c.lcsc, note:c.note, refs:[] };
  groups[key].refs.push(c.des);
}
const catOrder = ['电阻','电容','IC','三极管','保护','LED','机械键','开关','连接器','模组接口','测试点','机械','其它'];
const rows = Object.values(groups).sort((a,b)=> (catOrder.indexOf(a.cat)-catOrder.indexOf(b.cat)) || a.val.localeCompare(b.val));

function esc(s){return String(s==null?'':s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');}
function colName(n){let s='';n++;while(n>0){const m=(n-1)%26;s=String.fromCharCode(65+m)+s;n=Math.floor((n-1)/26);}return s;}
function sheetXml(headers, dataRows){
  let r=1; let out='<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><sheetData>';
  function row(cells,style){out+=`<row r="${r}">`;cells.forEach((v,i)=>{out+=`<c r="${colName(i)}${r}" t="inlineStr"${style?` s="${style}"`:''}><is><t xml:space="preserve">${esc(v)}</t></is></c>`;});out+='</row>';r++;}
  row(headers,1); for(const dr of dataRows) row(dr); out+='</sheetData></worksheet>'; return out;
}
const headers=['序号','类别','元件值/型号','封装','数量','位号(Designators)','立创编号(LCSC)','备注'];
const dataRows=rows.map((g,i)=>[i+1,g.cat,g.val,g.fp,g.refs.length,g.refs.join(', '),g.lcsc,g.note]);

const stylesXml='<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<styleSheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><fonts count="2"><font><sz val="11"/><name val="Calibri"/></font><font><b/><sz val="11"/><name val="Calibri"/></font></fonts><fills count="2"><fill><patternFill patternType="none"/></fill><fill><patternFill patternType="gray125"/></fill></fills><borders count="1"><border><left/><right/><top/><bottom/><diagonal/></border></borders><cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs><cellXfs count="2"><xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0"/><xf numFmtId="0" fontId="1" fillId="0" borderId="0" xfId="0" applyFont="1"/></cellXfs><cellStyles count="1"><cellStyle name="Normal" xfId="0" builtinId="0"/></cellStyles></styleSheet>';
const contentTypes='<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/><Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/><Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/><Override PartName="/xl/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/></Types>';
const rootRels='<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/></Relationships>';
const wbXml='<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><sheets><sheet name="BOM" sheetId="1" r:id="rId1"/></sheets></workbook>';
const wbRels='<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/><Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/></Relationships>';
const sheet1=sheetXml(headers,dataRows);
function crc32(buf){let c=~0;for(let i=0;i<buf.length;i++){c^=buf[i];for(let k=0;k<8;k++)c=(c>>>1)^(0xEDB88320&(-(c&1)));}return ~c>>>0;}
const files=[['[Content_Types].xml',contentTypes],['_rels/.rels',rootRels],['xl/workbook.xml',wbXml],['xl/_rels/workbook.xml.rels',wbRels],['xl/styles.xml',stylesXml],['xl/worksheets/sheet1.xml',sheet1]];
const central=[];const chunks=[];let offset=0;
for(const [name,content] of files){const data=Buffer.from(content,'utf8');const comp=zlib.deflateRawSync(data);const crc=crc32(data);const nameBuf=Buffer.from(name,'utf8');const lh=Buffer.alloc(30);lh.writeUInt32LE(0x04034b50,0);lh.writeUInt16LE(20,4);lh.writeUInt16LE(0,6);lh.writeUInt16LE(8,8);lh.writeUInt16LE(0,10);lh.writeUInt16LE(0,12);lh.writeUInt32LE(crc,14);lh.writeUInt32LE(comp.length,18);lh.writeUInt32LE(data.length,22);lh.writeUInt16LE(nameBuf.length,26);lh.writeUInt16LE(0,28);chunks.push(lh,nameBuf,comp);const ch=Buffer.alloc(46);ch.writeUInt32LE(0x02014b50,0);ch.writeUInt16LE(20,4);ch.writeUInt16LE(20,6);ch.writeUInt16LE(0,8);ch.writeUInt16LE(8,10);ch.writeUInt16LE(0,12);ch.writeUInt16LE(0,14);ch.writeUInt32LE(crc,16);ch.writeUInt32LE(comp.length,20);ch.writeUInt32LE(data.length,24);ch.writeUInt16LE(nameBuf.length,28);ch.writeUInt16LE(0,30);ch.writeUInt16LE(0,32);ch.writeUInt16LE(0,34);ch.writeUInt16LE(0,36);ch.writeUInt32LE(0,38);ch.writeUInt32LE(offset,42);central.push(ch,nameBuf);offset+=lh.length+nameBuf.length+comp.length;}
const centralBuf=Buffer.concat(central);const filesBuf=Buffer.concat(chunks);const eocd=Buffer.alloc(22);eocd.writeUInt32LE(0x06054b50,0);eocd.writeUInt16LE(0,4);eocd.writeUInt16LE(0,6);eocd.writeUInt16LE(files.length,8);eocd.writeUInt16LE(files.length,10);eocd.writeUInt32LE(centralBuf.length,12);eocd.writeUInt32LE(filesBuf.length,16);eocd.writeUInt16LE(0,20);
const zip=Buffer.concat([filesBuf,centralBuf,eocd]);
const outPath='C:/@MyWorks/21_HardWorks/agentic_keyboard_with_tuya/hardware/BOM_AgenticKeyboard_ExtBoard.xlsx';
fs.writeFileSync(outPath,zip);
console.log(JSON.stringify({file:outPath,groups:rows.length,totalRefs:comps.length}));
