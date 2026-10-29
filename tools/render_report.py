"""Export the report source to PDF. Does not compile firmware or run hardware."""
from pathlib import Path
import re
from xml.sax.saxutils import escape
from reportlab.pdfgen import canvas
from reportlab.platypus import SimpleDocTemplate, Paragraph, Spacer, Table, TableStyle, PageBreak, KeepTogether
from reportlab.lib import colors
from reportlab.lib.styles import getSampleStyleSheet, ParagraphStyle
from reportlab.lib.enums import TA_JUSTIFY
from reportlab.lib.pagesizes import A4
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont

ROOT = Path(__file__).resolve().parents[1]
pdfmetrics.registerFont(TTFont('Arial', 'C:/Windows/Fonts/arial.ttf'))
pdfmetrics.registerFont(TTFont('Arial-Bold', 'C:/Windows/Fonts/arialbd.ttf'))
styles = getSampleStyleSheet()
styles.add(ParagraphStyle('BodyPT', fontName='Arial', fontSize=9.5, leading=13, spaceAfter=7, alignment=TA_JUSTIFY))
styles.add(ParagraphStyle('TitlePT', fontName='Arial-Bold', fontSize=18, leading=22, spaceAfter=14, textColor=colors.HexColor('#15344D')))
styles.add(ParagraphStyle('HeadPT', fontName='Arial-Bold', fontSize=12, leading=16, spaceBefore=10, spaceAfter=8, keepWithNext=True))
styles.add(ParagraphStyle('CellPT', fontName='Arial', fontSize=8, leading=10))
styles.add(ParagraphStyle('CellHeadPT', fontName='Arial-Bold', fontSize=8, leading=10, textColor=colors.white))

def markup(text):
    return re.sub(r'\*\*(.*?)\*\*',r'<b>\1</b>',escape(text)).replace('`','')

def footer(c, doc):
    width, height = A4
    c.setStrokeColor(colors.HexColor('#CAD4DC')); c.line(70,43,width-57,43)
    c.setFont('Arial',8); c.setFillColor(colors.HexColor('#536370'))
    c.drawString(70,30,'STR M2 | Relatório-base | Medições pendentes')
    c.drawRightString(width-57,30,str(doc.page))

def export():
    out=ROOT/'output/pdf/relatorio-m2-base.pdf'; out.parent.mkdir(parents=True,exist_ok=True)
    lines=(ROOT/'docs/m2/RELATORIO.md').read_text(encoding='utf-8').splitlines()
    story=[]; i=0
    while i<len(lines):
        line=lines[i].strip()
        if not line: i+=1; continue
        if line=='---': story.append(PageBreak()); i+=1; continue
        if line.startswith('|'):
            rows=[]
            while i<len(lines) and lines[i].strip().startswith('|'):
                row=[cell.strip() for cell in lines[i].strip().strip('|').split('|')]
                if not all(set(cell)<=set('-: ') for cell in row): rows.append(row)
                i+=1
            count=len(rows[0]); width=A4[0]-127
            # Give task names and text columns adequate space.
            if count==5: fractions=[.20,.24,.18,.16,.22]
            elif count==7: fractions=[.23]+[.77/6]*6
            elif count==2: fractions=[.36,.64]
            else: fractions=[1/count]*count
            data=[[Paragraph(markup(cell),styles['CellHeadPT' if r==0 else 'CellPT']) for cell in row] for r,row in enumerate(rows)]
            table=Table(data,colWidths=[width*x for x in fractions],repeatRows=1,hAlign='LEFT')
            table.setStyle(TableStyle([
                ('BACKGROUND',(0,0),(-1,0),colors.HexColor('#15344D')),
                ('ROWBACKGROUNDS',(0,1),(-1,-1),[colors.HexColor('#EFF3F6'),colors.white]),
                ('VALIGN',(0,0),(-1,-1),'TOP'),('LEFTPADDING',(0,0),(-1,-1),6),
                ('RIGHTPADDING',(0,0),(-1,-1),6),('TOPPADDING',(0,0),(-1,-1),7),
                ('BOTTOMPADDING',(0,0),(-1,-1),7),('LINEBELOW',(0,-1),(-1,-1),.5,colors.HexColor('#CAD4DC'))]))
            story.extend([table,Spacer(1,9)]); continue
        if line.startswith('# '): story.append(Paragraph(markup(line[2:]),styles['TitlePT']))
        elif line.startswith('## '): story.append(Paragraph(markup(line[3:]),styles['HeadPT']))
        else:
            parts=[line]; i+=1
            while i<len(lines) and lines[i].strip() and not lines[i].startswith(('#','|','---')):
                parts.append(lines[i].strip()); i+=1
            story.append(Paragraph(markup(' '.join(parts)),styles['BodyPT'])); continue
        i+=1
    SimpleDocTemplate(str(out),pagesize=A4,leftMargin=70,rightMargin=57,topMargin=48,bottomMargin=58,
        title='Esteira industrial - STR M2 - Relatório-base',author='Guilherme Santana e Thiago Bazilio').build(story,onFirstPage=footer,onLaterPages=footer)
    print(out)

if __name__ == '__main__':
    export()
