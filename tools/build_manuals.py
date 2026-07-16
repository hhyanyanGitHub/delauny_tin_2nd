"""Generate the two dterrain Word manuals.

Run with the bundled workspace Python that provides python-docx.
"""

from __future__ import annotations

from datetime import date
from pathlib import Path

from docx import Document
from docx.enum.section import WD_SECTION
from docx.enum.style import WD_STYLE_TYPE
from docx.enum.table import WD_ALIGN_VERTICAL, WD_TABLE_ALIGNMENT
from docx.enum.text import WD_ALIGN_PARAGRAPH, WD_BREAK, WD_LINE_SPACING
from docx.oxml import OxmlElement
from docx.oxml.ns import qn
from docx.shared import Inches, Pt, RGBColor


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "docs" / "manuals"
VERSION = "0.7.0"
TODAY = date(2026, 7, 16).isoformat()

BLUE = "2E74B5"
DARK_BLUE = "1F4D78"
NAVY = "203748"
MUTED = "66727D"
LIGHT_BLUE = "E8EEF5"
LIGHT_GRAY = "F2F4F7"
CALLOUT = "F4F6F9"
WHITE = "FFFFFF"
INK = "20262C"
GREEN = "2F6B4F"
GOLD = "7A5A00"
RED = "9B1C1C"


def set_run_font(run, size=None, bold=None, italic=None, color=None,
                 latin="Calibri", east_asia="Microsoft YaHei"):
    run.font.name = latin
    run._element.get_or_add_rPr().rFonts.set(qn("w:ascii"), latin)
    run._element.get_or_add_rPr().rFonts.set(qn("w:hAnsi"), latin)
    run._element.get_or_add_rPr().rFonts.set(qn("w:eastAsia"), east_asia)
    if size is not None:
        run.font.size = Pt(size)
    if bold is not None:
        run.bold = bold
    if italic is not None:
        run.italic = italic
    if color is not None:
        run.font.color.rgb = RGBColor.from_string(color)


def set_cell_margins(cell, top=80, start=120, bottom=80, end=120):
    tc_pr = cell._tc.get_or_add_tcPr()
    tc_mar = tc_pr.first_child_found_in("w:tcMar")
    if tc_mar is None:
        tc_mar = OxmlElement("w:tcMar")
        tc_pr.append(tc_mar)
    for tag, value in (("top", top), ("start", start),
                       ("bottom", bottom), ("end", end)):
        element = tc_mar.find(qn(f"w:{tag}"))
        if element is None:
            element = OxmlElement(f"w:{tag}")
            tc_mar.append(element)
        element.set(qn("w:w"), str(value))
        element.set(qn("w:type"), "dxa")


def shade_cell(cell, fill):
    tc_pr = cell._tc.get_or_add_tcPr()
    shd = tc_pr.find(qn("w:shd"))
    if shd is None:
        shd = OxmlElement("w:shd")
        tc_pr.append(shd)
    shd.set(qn("w:fill"), fill)


def set_repeat_table_header(row):
    tr_pr = row._tr.get_or_add_trPr()
    tbl_header = OxmlElement("w:tblHeader")
    tbl_header.set(qn("w:val"), "true")
    tr_pr.append(tbl_header)


def set_table_geometry(table, widths_dxa, indent_dxa=120):
    table.alignment = WD_TABLE_ALIGNMENT.LEFT
    table.autofit = False
    tbl_pr = table._tbl.tblPr
    tbl_w = tbl_pr.find(qn("w:tblW"))
    if tbl_w is None:
        tbl_w = OxmlElement("w:tblW")
        tbl_pr.append(tbl_w)
    tbl_w.set(qn("w:w"), str(sum(widths_dxa)))
    tbl_w.set(qn("w:type"), "dxa")
    tbl_ind = tbl_pr.find(qn("w:tblInd"))
    if tbl_ind is None:
        tbl_ind = OxmlElement("w:tblInd")
        tbl_pr.append(tbl_ind)
    tbl_ind.set(qn("w:w"), str(indent_dxa))
    tbl_ind.set(qn("w:type"), "dxa")

    grid = table._tbl.tblGrid
    for child in list(grid):
        grid.remove(child)
    for width in widths_dxa:
        col = OxmlElement("w:gridCol")
        col.set(qn("w:w"), str(width))
        grid.append(col)

    for row in table.rows:
        for index, cell in enumerate(row.cells):
            width = widths_dxa[index]
            tc_pr = cell._tc.get_or_add_tcPr()
            tc_w = tc_pr.find(qn("w:tcW"))
            if tc_w is None:
                tc_w = OxmlElement("w:tcW")
                tc_pr.append(tc_w)
            tc_w.set(qn("w:w"), str(width))
            tc_w.set(qn("w:type"), "dxa")
            set_cell_margins(cell)
            cell.vertical_alignment = WD_ALIGN_VERTICAL.CENTER


def add_page_field(paragraph):
    run = paragraph.add_run()
    begin = OxmlElement("w:fldChar")
    begin.set(qn("w:fldCharType"), "begin")
    instr = OxmlElement("w:instrText")
    instr.set(qn("xml:space"), "preserve")
    instr.text = " PAGE "
    separate = OxmlElement("w:fldChar")
    separate.set(qn("w:fldCharType"), "separate")
    text = OxmlElement("w:t")
    text.text = "1"
    end = OxmlElement("w:fldChar")
    end.set(qn("w:fldCharType"), "end")
    run._r.extend([begin, instr, separate, text, end])
    set_run_font(run, size=9, color=MUTED)


def set_paragraph_shading(paragraph, fill):
    p_pr = paragraph._p.get_or_add_pPr()
    shd = p_pr.find(qn("w:shd"))
    if shd is None:
        shd = OxmlElement("w:shd")
        p_pr.append(shd)
    shd.set(qn("w:fill"), fill)


def set_keep_with_next(paragraph, value=True):
    paragraph.paragraph_format.keep_with_next = value


class Manual:
    def __init__(self, title, short_title, subtitle, audience):
        self.doc = Document()
        self.title = title
        self.short_title = short_title
        self.subtitle = subtitle
        self.audience = audience
        self._number_counter = 0
        self._configure_document()

    def _configure_document(self):
        doc = self.doc
        section = doc.sections[0]
        section.page_width = Inches(8.5)
        section.page_height = Inches(11)
        section.top_margin = Inches(1)
        section.right_margin = Inches(1)
        section.bottom_margin = Inches(1)
        section.left_margin = Inches(1)
        section.header_distance = Inches(0.492)
        section.footer_distance = Inches(0.492)
        section.different_first_page_header_footer = True

        styles = doc.styles
        normal = styles["Normal"]
        normal.font.name = "Calibri"
        normal._element.rPr.rFonts.set(qn("w:ascii"), "Calibri")
        normal._element.rPr.rFonts.set(qn("w:hAnsi"), "Calibri")
        normal._element.rPr.rFonts.set(qn("w:eastAsia"), "Microsoft YaHei")
        normal.font.size = Pt(11)
        normal.font.color.rgb = RGBColor.from_string(INK)
        normal.paragraph_format.space_before = Pt(0)
        normal.paragraph_format.space_after = Pt(6)
        normal.paragraph_format.line_spacing = 1.25
        normal.paragraph_format.widow_control = True

        for name, size, color, before, after in (
            ("Heading 1", 16, BLUE, 18, 10),
            ("Heading 2", 13, BLUE, 14, 7),
            ("Heading 3", 12, DARK_BLUE, 10, 5),
        ):
            style = styles[name]
            style.font.name = "Calibri"
            style._element.rPr.rFonts.set(qn("w:ascii"), "Calibri")
            style._element.rPr.rFonts.set(qn("w:hAnsi"), "Calibri")
            style._element.rPr.rFonts.set(qn("w:eastAsia"), "Microsoft YaHei")
            style.font.size = Pt(size)
            style.font.bold = True
            style.font.color.rgb = RGBColor.from_string(color)
            style.paragraph_format.space_before = Pt(before)
            style.paragraph_format.space_after = Pt(after)
            style.paragraph_format.keep_with_next = True
            style.paragraph_format.widow_control = True

        for name in ("List Bullet", "List Number"):
            style = styles[name]
            style.font.name = "Calibri"
            style._element.rPr.rFonts.set(qn("w:eastAsia"), "Microsoft YaHei")
            style.font.size = Pt(11)
            pf = style.paragraph_format
            pf.left_indent = Inches(0.375)
            pf.first_line_indent = Inches(-0.188)
            pf.space_after = Pt(4)
            pf.line_spacing = 1.25
            pf.widow_control = True

        code = styles.add_style("Dterrain Code", WD_STYLE_TYPE.PARAGRAPH)
        code.font.name = "Consolas"
        code._element.rPr.rFonts.set(qn("w:ascii"), "Consolas")
        code._element.rPr.rFonts.set(qn("w:hAnsi"), "Consolas")
        code._element.rPr.rFonts.set(qn("w:eastAsia"), "Microsoft YaHei")
        code.font.size = Pt(8.5)
        code.font.color.rgb = RGBColor.from_string("1E2933")
        code.paragraph_format.left_indent = Inches(0.18)
        code.paragraph_format.right_indent = Inches(0.18)
        code.paragraph_format.space_before = Pt(3)
        code.paragraph_format.space_after = Pt(7)
        code.paragraph_format.line_spacing = 1.0
        code.paragraph_format.keep_together = True

        caption = styles["Caption"]
        caption.font.name = "Calibri"
        caption._element.rPr.rFonts.set(qn("w:eastAsia"), "Microsoft YaHei")
        caption.font.size = Pt(9)
        caption.font.italic = True
        caption.font.color.rgb = RGBColor.from_string(MUTED)
        caption.paragraph_format.space_before = Pt(4)
        caption.paragraph_format.space_after = Pt(4)

        self._set_running_furniture(section)

    def _set_running_furniture(self, section):
        header = section.header
        p = header.paragraphs[0]
        p.alignment = WD_ALIGN_PARAGRAPH.LEFT
        p.paragraph_format.space_after = Pt(0)
        r = p.add_run(f"dterrain  |  {self.short_title}")
        set_run_font(r, size=9, bold=True, color=MUTED)

        footer = section.footer
        p = footer.paragraphs[0]
        p.alignment = WD_ALIGN_PARAGRAPH.RIGHT
        p.paragraph_format.space_before = Pt(0)
        p.paragraph_format.space_after = Pt(0)
        r = p.add_run(f"版本 {VERSION}  |  第 ")
        set_run_font(r, size=9, color=MUTED)
        add_page_field(p)
        r = p.add_run(" 页")
        set_run_font(r, size=9, color=MUTED)

    def cover(self, kicker):
        for _ in range(5):
            self.doc.add_paragraph().paragraph_format.space_after = Pt(10)
        p = self.doc.add_paragraph()
        p.alignment = WD_ALIGN_PARAGRAPH.CENTER
        p.paragraph_format.space_after = Pt(18)
        r = p.add_run(kicker.upper())
        set_run_font(r, size=10, bold=True, color=BLUE)

        p = self.doc.add_paragraph()
        p.alignment = WD_ALIGN_PARAGRAPH.CENTER
        p.paragraph_format.space_after = Pt(12)
        r = p.add_run(self.title)
        set_run_font(r, size=28, bold=True, color=NAVY)

        p = self.doc.add_paragraph()
        p.alignment = WD_ALIGN_PARAGRAPH.CENTER
        p.paragraph_format.space_after = Pt(44)
        r = p.add_run(self.subtitle)
        set_run_font(r, size=14, color=DARK_BLUE)

        p = self.doc.add_paragraph()
        p.alignment = WD_ALIGN_PARAGRAPH.CENTER
        p.paragraph_format.space_after = Pt(6)
        r = p.add_run(f"版本 {VERSION}  |  {TODAY}")
        set_run_font(r, size=11, bold=True, color=NAVY)

        p = self.doc.add_paragraph()
        p.alignment = WD_ALIGN_PARAGRAPH.CENTER
        p.paragraph_format.space_after = Pt(0)
        r = p.add_run(f"适用对象：{self.audience}")
        set_run_font(r, size=10, italic=True, color=MUTED)
        self.doc.add_page_break()

    def h1(self, text):
        self._number_counter = 0
        return self.doc.add_paragraph(text, style="Heading 1")

    def h2(self, text):
        self._number_counter = 0
        return self.doc.add_paragraph(text, style="Heading 2")

    def h3(self, text):
        self._number_counter = 0
        return self.doc.add_paragraph(text, style="Heading 3")

    def para(self, text="", bold_prefix=None):
        p = self.doc.add_paragraph()
        if bold_prefix and text.startswith(bold_prefix):
            r = p.add_run(bold_prefix)
            set_run_font(r, bold=True)
            r = p.add_run(text[len(bold_prefix):])
            set_run_font(r)
        else:
            r = p.add_run(text)
            set_run_font(r)
        return p

    def bullet(self, text):
        p = self.doc.add_paragraph(style="List Bullet")
        r = p.add_run(text)
        set_run_font(r)
        return p

    def number(self, text):
        self._number_counter += 1
        p = self.doc.add_paragraph()
        p.paragraph_format.left_indent = Inches(0.28)
        p.paragraph_format.first_line_indent = Inches(-0.20)
        r = p.add_run(f"{self._number_counter}.  {text}")
        set_run_font(r)
        return p

    def callout(self, label, text, tone="blue"):
        p = self.doc.add_paragraph()
        p.paragraph_format.left_indent = Inches(0.16)
        p.paragraph_format.right_indent = Inches(0.16)
        p.paragraph_format.space_before = Pt(5)
        p.paragraph_format.space_after = Pt(8)
        p.paragraph_format.line_spacing = 1.2
        fill = {"blue": LIGHT_BLUE, "gray": CALLOUT,
                "gold": "FFF7E0", "red": "FDECEC"}[tone]
        set_paragraph_shading(p, fill)
        r = p.add_run(label + "  ")
        set_run_font(r, bold=True,
                     color={"blue": DARK_BLUE, "gray": NAVY,
                            "gold": GOLD, "red": RED}[tone])
        r = p.add_run(text)
        set_run_font(r)
        return p

    def code(self, text):
        p = self.doc.add_paragraph(style="Dterrain Code")
        set_paragraph_shading(p, LIGHT_GRAY)
        lines = text.strip("\n").split("\n")
        for index, line in enumerate(lines):
            if index:
                p.add_run().add_break()
            r = p.add_run(line)
            set_run_font(r, size=8.5, latin="Consolas",
                         east_asia="Microsoft YaHei")
        return p

    def table(self, headers, rows, widths_dxa, alignments=None):
        table = self.doc.add_table(rows=1, cols=len(headers))
        table.style = "Table Grid"
        for index, header in enumerate(headers):
            cell = table.rows[0].cells[index]
            shade_cell(cell, LIGHT_BLUE)
            p = cell.paragraphs[0]
            p.paragraph_format.space_before = Pt(0)
            p.paragraph_format.space_after = Pt(0)
            p.alignment = WD_ALIGN_PARAGRAPH.CENTER
            r = p.add_run(str(header))
            set_run_font(r, size=9.5, bold=True, color=NAVY)
        set_repeat_table_header(table.rows[0])
        for row_values in rows:
            cells = table.add_row().cells
            for index, value in enumerate(row_values):
                p = cells[index].paragraphs[0]
                p.paragraph_format.space_before = Pt(0)
                p.paragraph_format.space_after = Pt(0)
                p.paragraph_format.line_spacing = 1.15
                if alignments:
                    p.alignment = alignments[index]
                r = p.add_run(str(value))
                set_run_font(r, size=9.2)
        set_table_geometry(table, widths_dxa)
        after = self.doc.add_paragraph()
        after.paragraph_format.space_before = Pt(0)
        after.paragraph_format.space_after = Pt(2)
        return table

    def metadata(self, rows):
        self.table(["项目", "说明"], rows, [2700, 6660],
                   [WD_ALIGN_PARAGRAPH.CENTER, WD_ALIGN_PARAGRAPH.LEFT])

    def new_page(self):
        self.doc.add_page_break()

    def save(self, path):
        path.parent.mkdir(parents=True, exist_ok=True)
        self.doc.core_properties.title = self.title
        self.doc.core_properties.subject = self.subtitle
        self.doc.core_properties.author = "dterrain project"
        self.doc.core_properties.keywords = "Delaunay,TIN,CGAL,terrain,DLL"
        self.doc.core_properties.comments = "Generated from the dterrain source tree."
        self.doc.save(path)


def add_document_control(m: Manual, scope, navigation):
    m.h1("文档说明")
    m.metadata([
        ("文档版本", VERSION),
        ("发布日期", TODAY),
        ("适用平台", "Windows x64；C++17；MinGW 或 Visual Studio"),
        ("适用工程", "dterrain 动态 2.5D Delaunay TIN 动态库及演示程序"),
        ("内容范围", scope),
    ])
    m.h2("阅读导航")
    for item in navigation:
        m.bullet(item)
    m.callout("版本边界", "本手册对应 dterrain 0.7.0：除普通 TIN、GRID、等高线和 GDAL 交换外，已增加独立约束 Delaunay 句柄、断裂线、外边界、孔洞、域内查询、DCDT 文本和 GUI 约束图层。百万点局部约束编辑和生产级 GPU LOD 属于后续阶段。", "gold")


def build_developer_manual():
    m = Manual(
        "dterrain 动态地形三角网 DLL\n开发与使用手册",
        "DLL 开发与使用手册",
        "面向测绘地形建模的 TIN、GRID、等高线转换、动态编辑与空间查询",
        "C/C++ 集成人员、算法研究人员、测试与运维人员",
    )
    m.cover("DEVELOPER REFERENCE")
    add_document_control(
        m,
        "架构、构建部署、稳定 C ABI、约束 Delaunay、GRID/等高线转换、GDAL 格式交换、异步任务、12 个兼容接口、文件格式、性能、线程安全、故障排查。",
        [
            "首次集成：重点阅读第 3、4、7 章。",
            "地形转换：重点阅读第 4.6 章和第 6 章。",
            "兼容既有系统：重点阅读第 5 章的 12 个 Legacy 接口。",
            "大数据量使用：重点阅读第 8 章的内存与查询策略。",
            "文件交换：重点阅读第 4.7 章和第 6 章。",
        ],
    )

    m.h1("1 产品概述")
    m.para("dterrain 是一个 C++17 动态库，在 XY 平面构建 Delaunay 三角网，并把 Z 作为顶点高程属性保存。它适用于局部坐标或投影坐标下的测绘地形建模、算法研究、动态编辑演示和既有系统集成。")
    m.h2("1.1 已实现能力")
    for text in (
        "从内存点数组或 XYZ/CSV 文本批量构建 Delaunay TIN。",
        "动态插入、按 XY 最近点删除、按稳定 ID 删除以及高程更新。",
        "返回编辑前后受影响三角形、边界边、删除边与新增边。",
        "最近顶点、点定位、矩形范围三角形查询、统计与完整校验。",
        "DTIN 二进制保存加载，以及 DTMESH 可读文本三角网交换。",
        "双精度 GRID、仿射节点坐标、NoData、窗口读写和 DGRID 文本往返。",
        "TIN→GRID、GRID→TIN，以及 TIN/GRID→等高线和 DCONTOUR 文本往返。",
        "耗时转换的异步任务、进度、等待、协作取消和结果提取。",
        "TIN/GRID/等高线 CRS WKT 元数据，以及可选 GeoTIFF/COG/GeoPackage 交换。",
        "独立 CDT 句柄、断裂线、外边界、孔洞、域内查询、约束增删与 DCDT 文本往返。",
        "推荐的稳定 C ABI 与原需求 12 个 C++ 接口兼容层。",
    ):
        m.bullet(text)
    m.h2("1.2 核心语义")
    m.table(
        ["主题", "规则"],
        [
            ("数据模型", "2.5D：Delaunay 判定只使用 XY；Z 是顶点高程属性。"),
            ("重复点", "相同 XY 被视为重复，返回 DT_E_DUPLICATE_XY。"),
            ("最近距离", "最近顶点和删除最近点都使用 XY 平面欧氏距离。"),
            ("高程更新", "dt_update_vertex_z() 只改变 Z，不改变网格拓扑。"),
            ("事务性", "批量建网和文件加载失败时保留原三角网。"),
        ("转换边界", "等高线是派生表达；当前尚未实现等高线反推 TIN/GRID。"),
        ],
        [2200, 7160],
        [WD_ALIGN_PARAGRAPH.CENTER, WD_ALIGN_PARAGRAPH.LEFT],
    )
    m.callout("适用边界", "GRID→TIN 仍重建普通 Delaunay；严格边界和孔洞应使用独立 dt_cdt_handle。v0.7 约束增删采用候选网全量重建，尚未达到百万点实时局部 CDT 编辑。", "gold")

    m.h1("2 架构与数据模型")
    m.h2("2.1 分层结构")
    m.code("""调用系统 / GUI
        |
稳定 C ABI + CDT API + Terrain API + Task API + GDAL API + Legacy 兼容层
        |
上下文、错误处理、结果句柄
        |
CGAL Delaunay hierarchy ---- Boost R-tree ---- 独立 Constrained Delaunay
        |                         |
顶点稳定 ID / Z              范围相交查询
        |
DTIN / DTMESH / XYZ / DGRID / DCONTOUR / DCDT 持久化
        |
GRID <---- 转换引擎 ----> TIN ----> 等高线
  \\------------- 可选 GDAL Adapter -------------/""")
    m.para("CGAL 顶点几何只保存 XY，自定义顶点信息保存 uint64_t 稳定 ID 和 Z。Boost R-tree 保存有限三角形的 XY 包围盒与面句柄。局部编辑时先移除旧面索引项，再加入新面，因此不需要每次重建全局范围索引。")
    m.h2("2.2 关键技术选择")
    m.table(
        ["组件", "用途", "工程影响"],
        [
            ("CGAL EPICK", "精确谓词、非精确构造", "提高方向和空圆判定鲁棒性。"),
            ("Delaunay_triangulation_2", "二维 Delaunay 拓扑", "Z 不进入三角剖分判定。"),
            ("Triangulation_hierarchy_2", "分层点定位", "有利于动态插入和空间定位。"),
            ("Boost R-tree", "面包围盒索引", "加速当前视口和矩形范围查询。"),
            ("Terrain Core", "GRID 与等高线转换", "不向 DLL 外暴露 C++ 容器和 CGAL 类型。"),
            ("Task Runtime", "后台转换任务", "保持源对象生命周期并提供协作取消。"),
            ("Constrained Delaunay", "断裂线与域裁剪", "独立句柄保持普通 TIN ABI 和性能语义。"),
        ],
        [2400, 2500, 4460],
        [WD_ALIGN_PARAGRAPH.CENTER, WD_ALIGN_PARAGRAPH.LEFT, WD_ALIGN_PARAGRAPH.LEFT],
    )

    m.new_page()
    m.h1("3 获取、构建与部署")
    m.h2("3.1 依赖")
    m.table(
        ["依赖", "最低/建议版本", "说明"],
        [
            ("CMake", "3.24+", "生成构建系统与安装目录。"),
            ("C++ 编译器", "C++17", "MinGW-w64 GCC 或 Visual Studio 2022 x64。"),
            ("CGAL", "6.x", "核心 Delaunay 后端。"),
            ("Boost", "随 CGAL", "R-tree 与 CGAL 依赖。"),
            ("GMP/MPFR", "随 vcpkg", "CGAL 数值依赖。"),
        ],
        [1800, 2200, 5360],
        [WD_ALIGN_PARAGRAPH.CENTER, WD_ALIGN_PARAGRAPH.CENTER, WD_ALIGN_PARAGRAPH.LEFT],
    )
    m.h2("3.2 MinGW + vcpkg 构建")
    m.code("""cmake -S . -B build -G "MinGW Makefiles" ^
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-mingw-dynamic ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DDT_BUILD_TESTS=ON -DDT_BUILD_EXAMPLES=ON -DDT_BUILD_GUI=ON

cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
cmake --install build --prefix dist""")
    m.h2("3.3 Visual Studio 构建")
    m.para("将生成器改为 Visual Studio 2022，并使用 x64-windows triplet。调用程序、DLL 和第三方运行库必须使用兼容的架构与运行时设置，避免混用 x86/x64 或不同 CRT。")
    m.h2("3.4 分发清单")
    m.table(
        ["文件", "是否必须", "用途"],
        [
            ("dterrain.dll", "是", "动态库实现。"),
            ("dt_api.h", "开发期", "推荐的稳定 C ABI。"),
            ("dt_cdt_api.h", "按需", "约束 Delaunay、边界与孔洞接口。"),
            ("dt_terrain_api.h", "开发期", "GRID、等高线和同步转换接口。"),
            ("dt_task_api.h", "按需", "异步转换任务接口。"),
            ("dt_legacy.hpp", "按需", "12 个原始接口兼容声明。"),
            ("libgmp-10.dll", "MinGW 包", "CGAL/GMP 运行库。"),
            ("libwinpthread-1.dll", "MinGW 包", "线程运行库。"),
            ("libdterrain.dll.a / .lib", "链接期", "导入库。"),
        ],
        [3300, 1700, 4360],
        [WD_ALIGN_PARAGRAPH.LEFT, WD_ALIGN_PARAGRAPH.CENTER, WD_ALIGN_PARAGRAPH.LEFT],
    )
    m.callout("部署建议", "优先直接复制 dist/bin、dist/include 和 dist/lib 的对应内容。不要只复制 dterrain.dll 而遗漏 MinGW 运行库。", "blue")

    m.h1("4 稳定 C ABI 集成")
    m.h2("4.1 生命周期")
    m.code("""#include "dt_api.h"

dt_handle mesh = nullptr;
if (dt_create(nullptr, &mesh) != DT_OK) {
    // 调用 dt_get_last_error() 获取原因
    return;
}

// 使用 mesh
dt_destroy(mesh);""")
    m.para("每个 dt_handle 表示一个独立网格上下文。接口不会把 C++ 异常传播到 DLL 外部。dt_destroy() 接受有效句柄并释放其全部网格和索引资源。")
    m.h2("4.2 批量建网")
    m.code("""dt_point3 points[] = {
    {0.0, 0.0, 10.0},
    {100.0, 0.0, 12.5},
    {0.0, 100.0, 20.0},
    {100.0, 100.0, 28.0}
};
dt_vertex_id ids[4]{};
dt_status s = dt_build(mesh, points, 4, ids);""")
    m.para("output_ids 可以为 NULL。成功后整体替换当前网格；输入非法、重复 XY 或内存不足时返回错误并保留旧网格。百万级以上点集应优先使用 dt_build() 或 dt_import_points_text()，不要逐点调用插入接口。")
    m.h2("4.3 动态编辑和影响结果")
    m.code("""dt_point3 p{50.0, 40.0, 18.0};
dt_vertex_id id = 0;
dt_edit_result result = nullptr;

if (dt_insert_point(mesh, &p, &id, &result) == DT_OK) {
    dt_edit_result_view view{};
    dt_edit_result_get_view(result, &view);
    // 消费 removed_triangles / boundary_edges / added_edges 等
}
dt_release_edit_result(result);""")
    m.table(
        ["结果字段", "含义", "有效期"],
        [
            ("removed_triangles", "编辑前被移除的有限三角形", "结果句柄释放前"),
            ("added_triangles", "编辑后新增的有限三角形", "结果句柄释放前"),
            ("boundary_edges", "局部影响区边界", "结果句柄释放前"),
            ("removed_edges", "旧线框中被删除的边", "结果句柄释放前"),
            ("added_edges", "新线框中新增的边", "结果句柄释放前"),
            ("affected_vertex_id", "插入或删除所影响的稳定顶点 ID", "值复制后长期有效"),
        ],
        [2700, 4300, 2360],
        [WD_ALIGN_PARAGRAPH.LEFT, WD_ALIGN_PARAGRAPH.LEFT, WD_ALIGN_PARAGRAPH.CENTER],
    )

    m.h2("4.4 查询")
    m.table(
        ["接口", "输入", "返回"],
        [
            ("dt_find_nearest_vertex_xy", "查询 XYZ；只使用 XY", "最近顶点坐标和 ID"),
            ("dt_locate_point_xy", "查询 XYZ；只使用 XY", "面、边、顶点或凸包外分类"),
            ("dt_query_triangles", "闭合 XY 矩形", "与矩形相交的有限三角形结果句柄"),
            ("dt_get_statistics", "网格句柄", "顶点数、面数、维度、范围、generation"),
            ("dt_validate", "网格句柄与详细级别", "CGAL 拓扑与 Delaunay 完整校验"),
        ],
        [3300, 2700, 3360],
        [WD_ALIGN_PARAGRAPH.LEFT, WD_ALIGN_PARAGRAPH.LEFT, WD_ALIGN_PARAGRAPH.LEFT],
    )
    m.code("""dt_bounds2 box{1000.0, 2000.0, 1200.0, 2200.0};
dt_query_result result = nullptr;
if (dt_query_triangles(mesh, &box, &result) == DT_OK) {
    dt_query_result_view view{};
    dt_query_result_get_view(result, &view);
    // view.triangles[0 .. triangle_count-1]
}
dt_release_query_result(result);""")
    m.callout("大范围查询", "范围结果一次性保存在结果对象中。千万级全图可能产生非常大的三角形数组；显示程序应按当前视口查询，必要时再在渲染层抽样。", "gold")

    m.h2("4.5 文件接口")
    m.table(
        ["接口", "格式", "用途"],
        [
            ("dt_import_points_text", "XYZ/TXT/CSV", "读取散点并立即自动构网。"),
            ("dt_save_mesh_text", "DTMESH 1", "保存顶点、稳定 ID 和显式三角形。"),
            ("dt_load_mesh_text", "DTMESH 1", "重建并逐面验证 Delaunay 拓扑。"),
            ("dt_save / dt_load", "DTIN v1", "紧凑二进制点集保存加载。"),
            ("dt_grid_save/load_text", "DGRID 1", "规则高程节点文本往返。"),
            ("dt_contours_save/load_text", "DCONTOUR 1", "等高折线文本往返。"),
            ("dt_cdt_save/load_text", "DCDT 1", "散点、约束和 CRS 文本往返。"),
        ],
        [3200, 1800, 4360],
        [WD_ALIGN_PARAGRAPH.LEFT, WD_ALIGN_PARAGRAPH.CENTER, WD_ALIGN_PARAGRAPH.LEFT],
    )
    m.code("""dt_bounds2 loaded{};
dt_status s = dt_import_points_text(
    mesh, "sample_data/sample_points.xyz", &loaded);
if (s != DT_OK) {
    char error[512]{};
    dt_get_last_error(error, sizeof(error), nullptr);
}""")

    m.h2("4.6 GRID、等高线与异步转换")
    m.para("新增接口分别位于 dt_terrain_api.h 和 dt_task_api.h。GRID 节点采用六参数仿射变换定位；TIN→GRID 在覆盖三角面上进行分片线性插值，凸包外写入 NoData。")
    m.code("""dt_tin_to_grid_options options{};
options.struct_size = sizeof(options);
options.width = 1001;
options.height = 1001;
options.geo_transform[0] = xmin;
options.geo_transform[1] = (xmax - xmin) / 1000.0;
options.geo_transform[3] = ymin;
options.geo_transform[5] = (ymax - ymin) / 1000.0;
options.nodata_value = -9999.0;

dt_grid_handle grid = nullptr;
dt_grid_from_tin(mesh, &options, &grid);
dt_grid_destroy(grid);""")
    m.table(
        ["接口组", "主要功能", "释放方式"],
        [
            ("dt_grid_*", "GRID 创建、信息、窗口读写、文本往返", "dt_grid_destroy"),
            ("dt_*_from_*", "TIN/GRID 同步转换和等高线生成", "按输出句柄释放"),
            ("dt_task_*", "异步启动、进度、等待、取消、结果提取", "dt_task_destroy"),
            ("dt_contours_*", "等高线信息、逐线视图和文本往返", "dt_contours_destroy"),
        ],
        [2300, 4700, 2360],
        [WD_ALIGN_PARAGRAPH.LEFT, WD_ALIGN_PARAGRAPH.LEFT, WD_ALIGN_PARAGRAPH.CENTER],
    )
    m.callout("NoData", "GRID→TIN 遇到 NoData 默认返回 DT_E_UNSUPPORTED。DT_GRID_TO_TIN_ALLOW_NODATA_BRIDGING 只适合调用方明确接受跨空洞三角化的场景。", "gold")
    m.code("""dt_task_handle task = nullptr;
dt_grid_from_tin_async(mesh, &options, &task);
int32_t completed = 0;
dt_task_wait(task, UINT32_MAX, &completed);

dt_task_info info{};
dt_task_get_info(task, &info);
if (info.state == DT_TASK_SUCCEEDED) {
    dt_grid_handle result = nullptr;
    dt_task_get_grid_result(task, &result);
    dt_grid_destroy(result);
}
dt_task_destroy(task);""")

    m.h2("4.7 CRS 与 GDAL 格式交换")
    m.para("dt_gdal_api.h 是可选适配层。默认 DT_WITH_GDAL=OFF 时函数仍然导出，但返回 DT_E_UNSUPPORTED；启用后可导入导出 GeoTIFF/COG GRID 和 GeoPackage 等高线。TIN、GRID 与等高线的 CRS 以 UTF-8 WKT 保存并在转换时传播，当前不执行坐标重投影。")
    m.code("""#include "dt_gdal_api.h"

dt_gdal_raster_save_options save{};
save.struct_size = sizeof(save);
save.driver_name = "COG";
const char* co[] = {"COMPRESS=DEFLATE", nullptr};
save.creation_options = co;
dt_grid_save_gdal_raster(grid, "terrain.tif", &save);

dt_grid_handle loaded = nullptr;
dt_grid_load_gdal_raster("terrain.tif", nullptr, &loaded);
dt_grid_destroy(loaded);""")
    m.table(
        ["对象", "格式/驱动", "保真内容"],
        [
            ("GRID", "GTiff / COG", "double 高程、NoData、仿射变换、CRS WKT"),
            ("等高线", "GPKG", "LineStringZ、elevation、closed、CRS WKT"),
            ("坐标换算", "GDAL ↔ GRID", "像元角点与节点中心自动进行半像元偏移"),
        ],
        [2000, 2500, 4860],
        [WD_ALIGN_PARAGRAPH.LEFT, WD_ALIGN_PARAGRAPH.LEFT, WD_ALIGN_PARAGRAPH.LEFT],
    )
    m.callout("部署", "启用 GDAL 的便携目录必须携带构建目录复制出的 GDAL、PROJ、SQLite、GeoTIFF 等运行时 DLL，以及 DLL 同级 share/proj/proj.db。COG 使用 CreateCopy，会比普通 GTiff 需要更多临时内存/存储。", "gold")

    m.h2("4.8 约束 Delaunay")
    m.para("dt_cdt_api.h 提供独立 dt_cdt_handle。基础点与普通 TIN 相互独立；断裂线只强制成边，外边界和孔洞通过奇偶嵌套层级决定有效域。没有外边界时全部有限面均为有效域。")
    m.code("""#include "dt_cdt_api.h"

dt_cdt_handle cdt = nullptr;
dt_cdt_create(nullptr, &cdt);
dt_cdt_build(cdt, points, point_count);
dt_cdt_add_constraint(cdt, DT_CONSTRAINT_OUTER_BOUNDARY, 0,
                      boundary, boundary_count, nullptr);
dt_cdt_add_constraint(cdt, DT_CONSTRAINT_HOLE_BOUNDARY, 0,
                      hole, hole_count, nullptr);
dt_cdt_save_text(cdt, "terrain.dcdt");
dt_cdt_destroy(cdt);""")
    m.table(
        ["类型", "作用", "闭合规则"],
        [
            ("BREAKLINE", "强制三角网沿折线分边，不改变域内外", "可开可闭"),
            ("OUTER_BOUNDARY", "地形有效域外边界", "自动闭合"),
            ("HOLE_BOUNDARY", "从外边界域中排除孔洞", "自动闭合；必须已有外边界"),
        ],
        [2600, 4300, 2460],
        [WD_ALIGN_PARAGRAPH.CENTER, WD_ALIGN_PARAGRAPH.LEFT, WD_ALIGN_PARAGRAPH.LEFT],
    )
    m.callout("性能与交叉", "v0.7 添加或删除约束会在候选状态中完整重建 CDT，成功后原子替换。未分段交叉必须先在交点处加入共享顶点，否则返回 DT_E_UNSUPPORTED。", "gold")

    m.h1("5 原 12 个接口兼容层")
    m.para("include/dt_legacy.hpp 提供原需求中的 12 个 C++ 接口。它们使用 DLL 内部的全局默认上下文，适合保持既有调用代码不变。新系统优先使用 dt_api.h，以获得多上下文、明确状态码和结果句柄。")
    legacy_rows = [
        ("1", "dt_init_dll", "初始化默认上下文；与 dt_free_dll 配对。"),
        ("2", "dt_free_dll", "释放默认上下文及其资源。"),
        ("3", "dt_insert_a_point_with_draw", "插点并返回受影响面、边界和新增边。"),
        ("4", "dt_insert_a_point", "仅插入 XYZ 点。"),
        ("5", "dt_delete_a_point_with_draw", "删除查询位置 XY 最近顶点并返回影响数据。"),
        ("6", "dt_delete_a_point", "仅删除查询位置 XY 最近顶点。"),
        ("7", "dt_clear", "清空默认三角网。"),
        ("8", "dt_save_triangulation", "保存 DTIN 二进制三角网。"),
        ("9", "dt_load_triangulation", "加载 DTIN 并返回 XY 范围。"),
        ("10", "dt_view_to_range", "返回与指定 XY 矩形相交的三角形。"),
        ("11", "dt_get_a_point_nearest_point", "返回查询位置 XY 最近顶点。"),
        ("12", "dt_get_a_triangle_covers_point", "返回平面投影严格包含查询点的三角形。"),
    ]
    m.table(["序号", "函数", "行为"], legacy_rows, [850, 3650, 4860],
            [WD_ALIGN_PARAGRAPH.CENTER, WD_ALIGN_PARAGRAPH.LEFT, WD_ALIGN_PARAGRAPH.LEFT])
    m.h2("5.1 pEffect 数组布局")
    m.para("带绘制结果的接口输出 f、h、e 和 pEffect。pEffect 中依次放置 f 个受影响三角形、h 条边界边、e 条新增边；每个空间点由连续三个 double 表示。")
    m.code("""对象数量 = f * 3 + (h + e) * 2
double 数量 = 3 * [f * 3 + (h + e) * 2]

区段 1: f * 3 个 XYZ 点  -> 受影响三角形
区段 2: h * 2 个 XYZ 点  -> 影响区边界边
区段 3: e * 2 个 XYZ 点  -> 新增边""")
    m.callout("所有权", "旧接口返回的 double* 由 DLL 管理，调用方不得 delete/free。它只在同一线程下一次 Legacy 接口调用前有效，需要长期保存时必须立即复制。", "red")
    m.h2("5.2 Legacy 最小调用流程")
    m.code("""#include "dt_legacy.hpp"

dt_init_dll();
dt_insert_a_point(0.0, 0.0, 10.0);
dt_insert_a_point(100.0, 0.0, 12.0);
dt_insert_a_point(0.0, 100.0, 20.0);

double x, y, z;
bool ok = dt_get_a_point_nearest_point(
    x, y, z, 10.0, 15.0, 0.0);

dt_free_dll();""")

    m.h1("6 文本与二进制格式")
    m.h2("6.1 XYZ 散点文本")
    m.para("每个有效行恰好包含三个有限浮点数 x y z。支持空格、制表符、逗号、分号、空行、整行 # 注释和第三个坐标后的行尾注释。")
    m.code("""# x, y, z
500000.0, 3200000.0, 103.25
500010.0 3200000.0 104.10
500010.0;3200010.0;106.80 # ridge""")
    m.h2("6.2 DTMESH 1")
    m.code("""DTMESH 1
VERTICES 4
1 0 0 10
2 1 0 20
3 1 1 30
4 0 1 40
TRIANGLES 2
1 2 3
1 3 4""")
    for text in (
        "VERTICES 每行是 id x y z；ID 必须为非零 uint64_t。",
        "TRIANGLES 每行是三个顶点 ID。",
        "加载时从顶点重建 Delaunay，并检查面数、引用、重复面与拓扑。",
        "不匹配时返回 DT_E_CORRUPTED_DATA，原网保持不变。",
    ):
        m.bullet(text)
    m.h2("6.3 DTIN v1")
    m.para("DTIN v1 保存顶点 ID 和 XYZ，加载时重建有效 Delaunay 网。对于共圆点集，重建后可能选择另一条同样合法的对角线，因此 DTIN v1 不承诺逐面拓扑完全一致。需要文本可读和逐面校验时使用 DTMESH。")
    m.h2("6.4 DGRID 规则高程节点文本")
    m.para("DGRID 1 保存 SIZE、FLAGS、六参数 GEOTRANSFORM、NODATA 和按行优先排列的 VALUES。该格式用于测试、研究和简单交换；当前变换描述节点位置，不等同于 GDAL 像元左上角语义。")
    m.h2("6.5 DCONTOUR 等高线文本")
    m.para("DCONTOUR 1 由 LINES 计数和若干 LINE 记录组成。每条 LINE 保存等高值、闭合标志、点数及 XYZ 顶点。逐线视图中的点数组只在等高线句柄释放前有效。")
    m.h2("6.6 GeoTIFF、COG 与 GeoPackage")
    m.para("启用 DT_WITH_GDAL 后，dt_grid_load/save_gdal_raster 读写 GDAL 单波段高程栅格；dt_contours_load/save_gdal_vector 读写等高线矢量层。普通 GeoTIFF 逐行写入；COG 按驱动要求通过临时数据集 CreateCopy；GeoPackage 输出 LineStringZ 和 elevation/closed 字段。")
    m.h2("6.7 DCDT 约束网文本")
    m.para("DCDT 1 保存 CRS、基础 XYZ 点和约束记录。每条约束包含稳定 ID、类型、闭合标志、点数和 XYZ；加载时完整重建 CDT 并重新标记域，失败不会覆盖原对象。")

    m.h1("7 错误、内存与线程")
    m.h2("7.1 状态码")
    status_rows = [
        ("DT_OK", "成功"),
        ("DT_E_INVALID_ARGUMENT", "空指针、无效坐标、无效矩形或参数组合"),
        ("DT_E_NOT_INITIALIZED", "上下文或 Legacy 默认环境尚未初始化"),
        ("DT_E_DUPLICATE_XY", "输入或插入点与现有顶点具有相同 XY"),
        ("DT_E_EMPTY", "在空网格上执行需要数据的操作"),
        ("DT_E_NOT_FOUND", "目标顶点或对象不存在"),
        ("DT_E_IO", "文件打开、读取或写入失败"),
        ("DT_E_OUT_OF_MEMORY", "内存分配失败"),
        ("DT_E_CORRUPTED_DATA", "文件格式、计数或拓扑校验失败"),
        ("DT_E_STALE_QUERY", "结果与当前 generation 不一致"),
        ("DT_E_INTERNAL", "未分类内部异常"),
        ("DT_E_UNSUPPORTED", "当前后端不支持所请求语义，例如未允许的 NoData 空洞"),
        ("DT_E_CANCELLED", "异步地形任务收到协作取消请求"),
        ("DT_E_LIMIT_EXCEEDED", "GRID、等高层或文本对象数量超过安全限制"),
    ]
    m.table(["状态", "说明"], status_rows, [3200, 6160],
            [WD_ALIGN_PARAGRAPH.LEFT, WD_ALIGN_PARAGRAPH.LEFT])
    m.h2("7.2 错误文本")
    m.code("""char message[1024]{};
size_t required = 0;
dt_get_last_error(message, sizeof(message), &required);""")
    m.para("最后错误文本按线程保存。接口失败后应在同一线程尽快读取；后续 DLL 调用可能覆盖它。")
    m.h2("7.3 结果所有权")
    m.table(
        ["对象", "创建者", "释放方式"],
        [
            ("dt_handle", "dt_create", "dt_destroy"),
            ("dt_edit_result", "插入/删除接口", "dt_release_edit_result"),
            ("dt_query_result", "dt_query_triangles", "dt_release_query_result"),
            ("dt_grid_handle", "GRID 创建/加载/转换", "dt_grid_destroy"),
            ("dt_contour_handle", "等高线生成/加载", "dt_contours_destroy"),
            ("dt_task_handle", "异步转换启动", "dt_task_destroy"),
            ("dt_cdt_handle", "dt_cdt_create", "dt_cdt_destroy"),
            ("dt_cdt_query_result", "dt_cdt_query_triangles", "dt_cdt_release_query_result"),
            ("Legacy double*", "兼容层", "不得释放；立即复制"),
        ],
        [2500, 3100, 3760],
        [WD_ALIGN_PARAGRAPH.LEFT, WD_ALIGN_PARAGRAPH.LEFT, WD_ALIGN_PARAGRAPH.LEFT],
    )
    m.h2("7.4 并发")
    m.para("每个上下文有读写锁。但当前 MinGW + CGAL 6.0.1 构建由于 TLS ABI 兼容性问题定义 CGAL_HAS_NO_THREADS，并用全局递归互斥锁串行保护 CGAL 调用。因此接口线程安全不等于能够并行扩展吞吐。Legacy 接口还共享一个默认上下文。")
    m.callout("建议", "同一网格的业务写操作在应用层串行调度；查询线程应避免持有结果对象过久。需要跨网格并行时，优先评估 MSVC/更新 CGAL 工具链并重新进行并发验证。", "blue")

    m.h1("8 大数据性能与容量规划")
    m.table(
        ["点数", "有限三角形", "建网时间", "峰值工作集", "完整校验"],
        [
            ("100,000", "199,973", "0.21 s", "未单独记录", "0.02 s"),
            ("1,000,000", "1,999,962", "3.47 s", "460.5 MB", "0.36 s"),
            ("10,000,000", "19,999,951", "50.44 s", "4,553.2 MB", "3.39 s"),
        ],
        [1700, 1900, 1700, 2200, 1860],
        [WD_ALIGN_PARAGRAPH.CENTER] * 5,
    )
    m.para("数据来自 Windows x64 Release/MinGW、随机均匀 XY 点集，用于说明数量级，不是对任意硬件和坐标分布的固定承诺。")
    m.h2("8.1 使用建议")
    for text in (
        "百万级以上优先批量构建，提前验证可用内存和临时峰值。",
        "使用局部或投影坐标；避免 NaN、Inf 和重复 XY。",
        "按视口调用矩形查询，不要为显示一次性导出全部千万级三角形。",
        "保存交换时，DTIN 更紧凑；DTMESH 更可读但体积更大。",
        "全量 dt_validate() 适合导入验收、调试和离线检查，不必每帧调用。",
    ):
        m.bullet(text)

    m.h1("9 测试与验收")
    m.h2("9.1 自动化测试")
    m.code("""cmake --build build --config Release --parallel 4
ctest --test-dir build --output-on-failure""")
    m.para("测试覆盖生命周期、普通 TIN 动态编辑、DTIN/DTMESH/XYZ、TIN/GRID/等高线、异步任务，以及 CDT 外边界、孔洞、断裂线、交叉失败原子性、约束删除和 DCDT 往返。GDAL 构建还执行 GTiff、COG、GPKG 驱动探测和真实文件往返。")
    m.h2("9.2 集成验收清单")
    for text in (
        "DLL、导入库、头文件和运行库均为相同架构。",
        "三点、四点、重复 XY、共线点、空文件和非法文件均有预期结果。",
        "所有结果句柄在成功和失败路径上都得到释放。",
        "中文路径通过 UTF-8 传入新 API。",
        "百万级样本的建网时间、内存和局部查询满足目标系统预算。",
        "保存后重新加载，顶点数、范围、高程和查询结果通过核对。",
    ):
        m.bullet(text)

    m.h1("10 常见问题")
    faq = [
        ("DLL 无法加载", "检查 dterrain.dll、libgmp-10.dll、libwinpthread-1.dll 是否同目录，并确认 x64/x86 一致。"),
        ("链接不到符号", "包含 dt_api.h，并链接 libdterrain.dll.a 或 dterrain.lib；不要混用 MSVC 与 MinGW 导入库。"),
        ("导入返回重复 XY", "同一 XY 只能保留一个高程；先清洗数据或明确采用何种聚合规则。"),
        ("文本三角网打不开", "确认 DTMESH 1 头、计数、ID 引用和三角形集合均与 Delaunay 拓扑一致。"),
        ("全图查询内存大", "缩小 dt_bounds2 范围，按视口或瓦片分批查询。"),
        ("Z 改变后面没有变化", "这是设计行为：Delaunay 只使用 XY，Z 更新不改变拓扑。"),
        ("插入点失败", "检查有限坐标、重复 XY、初始化状态和 dt_get_last_error()。"),
        ("GDAL 接口不支持", "重新配置 DT_WITH_GDAL=ON，并确认 GDAL::GDAL 可被 CMake 找到。"),
        ("GDAL 版 DLL 无法加载", "将构建目录复制出的 GDAL/PROJ/SQLite/GeoTIFF 等依赖 DLL 与 dterrain.dll 放在同目录。"),
    ]
    m.table(["现象", "处理"], faq, [2600, 6760],
            [WD_ALIGN_PARAGRAPH.LEFT, WD_ALIGN_PARAGRAPH.LEFT])

    m.h1("11 许可证")
    m.para("当前后端使用 CGAL 2D Triangulations 包。该包采用 GPL 或商业许可双重模式；本工程定位为研究和演示用途。对外分发、闭源集成或用途改变前，应再次核查 CGAL 及依赖许可。")

    path = OUTPUT / "dterrain_DLL开发使用手册.docx"
    m.save(path)
    return path


def build_gui_manual():
    m = Manual(
        "dterrain GUI 演示程序\n操作手册与入门教程",
        "GUI 操作入门教程",
        "从 XYZ 散点导入到动态编辑、TIN/GRID/等高线转换、约束 Delaunay、三维漫游与数据保存",
        "首次使用者、演示人员、算法验证与测绘研究人员",
    )
    m.cover("QUICK START GUIDE")
    add_document_control(
        m,
        "便携运行、五分钟上手、控件与菜单、XYZ 导入、TIN/GRID/等高线/CDT 图层、2D/3D 浏览、查询编辑和数据保存。",
        [
            "第一次体验：直接完成第 2 章的五分钟教程。",
            "已有 XYZ 数据：重点阅读第 5 章。",
            "三维地形展示：重点阅读第 4 章。",
            "TIN/GRID/等高线转换与交换：重点阅读第 4.3 节和第 8 章。",
            "演示动态编辑：重点阅读第 7 章。",
            "百万级数据展示：重点阅读第 9 章。",
        ],
    )

    m.h1("1 程序概览")
    m.para("dterrain_demo.exe 是一个原生 Win32/GDI 演示程序，用于直观验证 dterrain.dll 的批量建网、范围查询、最近点查询、动态插入删除、编辑影响显示、文件交换和三维地形漫游。它不依赖 Qt 等 GUI 框架。")
    m.callout("0.7 功能边界", "当前程序已提供二维 TIN 编辑、透视三维 TIN 查看、TIN/GRID/等高线转换，以及 DCDT 约束网的打开、保存和二维图层检查。单条约束交互绘制/删除、GeoTIFF/GeoPackage 菜单和生产级 GPU/LOD 尚未接入。", "gold")
    m.h2("1.1 运行文件")
    m.table(
        ["文件", "作用"],
        [
            ("dterrain_demo.exe", "GUI 主程序。"),
            ("dterrain.dll", "动态 Delaunay TIN 功能。"),
            ("libgmp-10.dll", "CGAL/GMP 数值运行库。"),
            ("libwinpthread-1.dll", "MinGW 线程运行库。"),
            ("sample_data/sample_points.xyz", "随包提供的入门散点样例。"),
            ("sample_data/sample_grid.dgrid", "随包提供的规则 GRID 文本样例。"),
            ("sample_data/sample_contours.dcontour", "随包提供的等高线文本样例。"),
            ("sample_data/sample_constraints.dcdt", "含外边界、孔洞和断裂线的 CDT 样例。"),
        ],
        [3700, 5660],
        [WD_ALIGN_PARAGRAPH.LEFT, WD_ALIGN_PARAGRAPH.LEFT],
    )
    m.callout("启动", "打开 dist/bin/dterrain_demo.exe。四个运行文件应位于同一 bin 目录；无需安装程序。", "blue")
    m.h2("1.2 启动后的默认状态")
    m.para("程序启动时会自动生成约 10 万个模拟地形点并构网。画布显示按高程分级着色的三角形线框，状态栏给出当前模式、顶点数、三角形数、当前窗口查询数量和耗时。")

    m.h1("2 五分钟上手")
    m.callout("目标", "导入示例 XYZ，浏览和编辑 TIN，生成 GRID 与等高线，体验三维漫游，最后保存数据。", "gray")
    steps = [
        "启动 dist/bin/dterrain_demo.exe，等待默认 10 万点网格显示完成。",
        "单击“导入XYZ”，选择 dist/sample_data/sample_points.xyz。导入成功后程序立即自动构网并全图适屏。",
        "滚动鼠标滚轮放大或缩小；按住右键拖动进行平移。",
        "单击“框选放大”，在画布中按住左键拖出矩形，松开后选区自动适配窗口。",
        "单击“切换3D”，左键拖动环视、滚轮拉近，再用 WASD 漫游；单击“高程×1.0”观察垂直夸张。",
        "单击“切换2D”返回二维编辑视图。",
        "打开“地形转换”菜单，依次执行“TIN → GRID”和“从 GRID 生成等高线”，观察三类图层叠加。",
        "打开“图层”菜单，分别隐藏和显示 TIN、GRID、等高线，再用“全图”恢复联合范围。",
        "打开“数据交换→打开约束网 DCDT”，选择 sample_constraints.dcdt，观察孔洞和断裂线。",
        "单击“查询模式”，在网格内单击；观察白色最近顶点和洋红色覆盖三角形。",
        "单击“插入模式”，在网格中单击；观察红色旧面、黄色边界和绿色新增面/边。",
        "单击“删除模式”，在目标附近单击；程序删除 XY 最近顶点并显示局部变化。",
        "单击“保存网格”，保存为 terrain.dtmesh；然后单击“清空”和“打开网格”验证加载。",
    ]
    for step in steps:
        m.number(step)
    m.callout("完成标志", "状态栏可显示 TGCD；画布能叠加普通 TIN、GRID、等高线与约束网，并能重新打开 .dtmesh、.dgrid、.dcontour 或 .dcdt。", "blue")

    m.new_page()
    m.h1("3 界面组成")
    m.h2("3.1 顶部工具栏")
    control_rows = [
        ("模拟10万", "生成约 100,000 个起伏地形点并自动构网。"),
        ("模拟100万", "生成约 1,000,000 个点，用于大数据交互演示。"),
        ("清空", "删除当前网格全部数据。"),
        ("查询模式", "单击显示最近顶点和查询点所在三角形。"),
        ("插入模式", "单击插入一个按演示地形函数计算 Z 的新点。"),
        ("删除模式", "删除单击位置 XY 距离最近的顶点。"),
        ("框选放大", "左键拖出矩形，松开后按画布比例放大适屏。"),
        ("全图", "恢复到完整数据范围并适屏。"),
        ("导入XYZ", "导入 XYZ/TXT/CSV 散点并立即构网。"),
        ("保存网格", "保存 DTMESH 文本或 DTIN 二进制网格。"),
        ("打开网格", "打开并校验 DTMESH/TXT，或加载 DTIN。"),
        ("切换3D/切换2D", "在二维编辑视图和透视三维 TIN 视图之间切换。"),
        ("高程×n", "循环选择 0.5、1.0、1.5、2、3、5、8 倍垂直夸张。"),
    ]
    m.table(["控件", "功能"], control_rows, [2600, 6760],
            [WD_ALIGN_PARAGRAPH.CENTER, WD_ALIGN_PARAGRAPH.LEFT])
    m.h2("3.2 主画布与状态栏")
    m.para("主画布绘制当前视口范围内的地形图层。底部状态栏从左到右显示：2D/3D 视图、当前模式、顶点数、有限三角形数、当前窗口查询结果数、查询耗时或垂直夸张、T/G/C/D 可见图层标记，以及最近一次操作说明。")
    m.h2("3.3 菜单栏")
    m.table(
        ["菜单", "主要命令"],
        [
            ("地形转换", "TIN→GRID、GRID→TIN、从 TIN/GRID 自动生成等高线。"),
            ("数据交换", "导入或导出 DGRID、DCONTOUR 和 DCDT 文本。"),
            ("图层", "分别显示或隐藏 TIN、GRID、等高线和约束 Delaunay。"),
        ],
        [2600, 6760],
        [WD_ALIGN_PARAGRAPH.CENTER, WD_ALIGN_PARAGRAPH.LEFT],
    )

    m.h1("4 浏览与视图控制")
    m.table(
        ["操作", "输入", "结果"],
        [
            ("滚轮缩放", "在画布中滚动滚轮", "以鼠标位置为中心连续缩放。"),
            ("平移", "按住右键或中键拖动", "移动当前观察范围。"),
            ("框选放大", "选择模式后左键拖框", "选区放大并保持 XY 比例。"),
            ("取消框选", "拖动过程中按 Esc", "取消选择，不改变视图。"),
            ("恢复全图", "单击“全图”", "数据完整范围重新适屏。"),
        ],
        [2200, 3300, 3860],
        [WD_ALIGN_PARAGRAPH.CENTER, WD_ALIGN_PARAGRAPH.LEFT, WD_ALIGN_PARAGRAPH.LEFT],
    )
    m.h2("4.1 框选放大细节")
    for text in (
        "选择“框选放大”后，鼠标左键用于选择，不执行查询或编辑。",
        "拖动时显示青色虚线框；可以从任意方向拖动。",
        "松开后程序按画布纵横比扩展选区，避免地形图形被拉伸。",
        "宽或高小于 8 像素的误拖会被忽略。",
        "放大后仍保持框选模式，可连续选择更小区域；单击其他模式可退出。",
    ):
        m.bullet(text)

    m.new_page()
    m.h2("4.2 三维查看与漫游")
    m.para("单击“切换3D”后，完整 TIN 以透视方式显示；红、绿、蓝短轴分别代表 X、Y、Z。三维只负责查看，选择查询、插入、删除或框选放大时会自动返回二维，防止把透视屏幕坐标误当作地形 XY。")
    m.table(
        ["输入", "三维操作"],
        [
            ("左键拖动", "绕观察目标环视。"),
            ("右键/中键拖动", "沿相机屏幕平面平移。"),
            ("滚轮", "拉近或拉远相机。"),
            ("W/S 或 ↑/↓", "沿当前水平方向前进或后退。"),
            ("A/D 或 ←/→", "向左或向右漫游。"),
            ("Q/E", "提升或降低观察目标。"),
            ("+/-", "增加或降低垂直夸张。"),
            ("Home / 全图", "恢复三维适屏相机。"),
        ],
        [3200, 6160],
        [WD_ALIGN_PARAGRAPH.CENTER, WD_ALIGN_PARAGRAPH.LEFT],
    )
    m.callout("大坐标稳定性", "三维模型先减去数据中心并按 XY 范围归一化，再使用双精度相机与投影计算，可直接查看局部坐标或大数值投影坐标。", "blue")

    m.h2("4.3 TIN、GRID、等高线与 CDT 图层")
    m.para("TIN→GRID 自动按 TIN 范围建立最长边 401 节点规则网；TIN/GRID→等高线自动选择易读等高距。GRID→TIN 仍可能跨 NoData；需要严格外边界、孔洞或断裂线时，应打开独立 DCDT 约束网。")
    m.table(
        ["图层", "二维显示", "数据变化规则"],
        [
            ("TIN", "高程分级网线和编辑效果", "重新建网或动态编辑会使派生 GRID/等高线失效。"),
            ("GRID", "连续高程着色和青色边框", "GRID→TIN 保留源 GRID。"),
            ("等高线", "黄色普通线与浅色加粗抽样线", "源 TIN/GRID 改变后应重新生成。"),
            ("CDT", "紫色域内网、青色外边界、洋红孔洞、橙色断裂线", "独立打开、保存和显隐。"),
        ],
        [1600, 3000, 4760],
        [WD_ALIGN_PARAGRAPH.CENTER, WD_ALIGN_PARAGRAPH.LEFT, WD_ALIGN_PARAGRAPH.LEFT],
    )
    m.callout("全图与显隐", "“全图”按所有可见二维图层的联合范围适屏。状态栏 T/G/C/D 分别表示普通 TIN、GRID、等高线和约束 Delaunay。", "blue")

    m.h1("5 导入 XYZ 散点并自动构网")
    m.h2("5.1 支持格式")
    m.para("每个有效行包含 x、y、z 三个有限数值。支持空格、制表符、逗号和分号分隔；支持空行、整行 # 注释和行尾注释。文件扩展名可为 .xyz、.txt 或 .csv。")
    m.code("""# x, y, z
500000.0, 3200000.0, 103.25
500010.0 3200000.0 104.10
500010.0;3200010.0;106.80 # ridge""")
    m.h2("5.2 导入步骤")
    for step in (
        "单击“导入XYZ”。",
        "在文件对话框中选择散点文件。首次体验可选择 dist/sample_data/sample_points.xyz。",
        "等待鼠标恢复普通指针；大文件构网期间程序会显示等待指针。",
        "查看状态栏中的点数和耗时，确认网格自动全图适屏。",
        "使用查询模式抽查高程和覆盖三角形。",
    ):
        m.number(step)
    m.h2("5.3 导入失败的常见原因")
    m.table(
        ["提示/现象", "原因与处理"],
        [
            ("contains no points", "文件为空、全是注释或没有有效 XYZ 行。"),
            ("invalid XYZ", "某行字段不足、多余、不是数字或存在 NaN/Inf。"),
            ("duplicate XY", "两个点的 X 和 Y 相同；先清洗或聚合高程。"),
            ("cannot open", "路径、权限、文件占用或文件名转换问题。"),
            ("内存不足", "减少点数，关闭其他程序，使用 64 位 Release 版本。"),
        ],
        [3000, 6360],
        [WD_ALIGN_PARAGRAPH.LEFT, WD_ALIGN_PARAGRAPH.LEFT],
    )
    m.callout("安全性", "导入具有事务性：解析或建网失败时，原三角网不会被部分覆盖。", "blue")

    m.h1("6 查询地形对象")
    m.h2("6.1 最近顶点和覆盖三角形")
    for step in (
        "单击“查询模式”。",
        "在网格内单击目标位置。",
        "白色圆点表示 XY 平面最近顶点。",
        "洋红色粗线表示查询点所在或相交的三角形。",
        "状态栏显示查询 XY、最近顶点 ID、Z 和操作耗时。",
    ):
        m.number(step)
    m.callout("注意", "查询使用 XY 投影距离，鼠标点击没有输入 Z；状态栏显示的是最近网格顶点自身的 Z。", "gold")
    m.h2("6.2 凸包外查询")
    m.para("在三角网凸包外单击时，仍可能找到最近顶点，但没有严格覆盖查询点的有限三角形。此时不会显示洋红色覆盖面。")

    m.h1("7 动态插入与删除")
    m.h2("7.1 插入点")
    for step in (
        "进入“插入模式”。",
        "在需要增加细节的位置单击。",
        "程序使用点击 XY，并按演示地形函数计算 Z。",
        "观察局部三角网即时更新及状态栏的新顶点 ID。",
    ):
        m.number(step)
    m.h2("7.2 删除点")
    for step in (
        "进入“删除模式”。",
        "在要删除的顶点附近单击。",
        "程序删除 XY 距离最近的网格顶点，而不是任意空间点。",
        "观察删除影响区和重新三角化结果。",
    ):
        m.number(step)
    m.h2("7.3 编辑颜色")
    m.table(
        ["颜色", "对象", "含义"],
        [
            ("红色", "旧三角形", "编辑前被删除或受影响的面。"),
            ("黄色", "边界边", "局部冲突/影响区域边界。"),
            ("绿色", "新三角形/新边", "编辑后生成的局部拓扑。"),
            ("洋红色", "查询三角形", "查询点所在或相交的面。"),
            ("白色", "圆形顶点标记", "查询位置的最近顶点。"),
        ],
        [1500, 2900, 4960],
        [WD_ALIGN_PARAGRAPH.CENTER, WD_ALIGN_PARAGRAPH.LEFT, WD_ALIGN_PARAGRAPH.LEFT],
    )
    m.callout("演示数据的 Z", "GUI 插入模式的高程来自内置模拟地形函数，不能手工输入。如果要插入真实指定 Z，请由调用系统使用 DLL 的 dt_insert_point()。", "gold")

    m.h1("8 保存与打开地形数据")
    m.h2("8.1 DTMESH 文本")
    m.para("保存扩展名为 .dtmesh 或 .txt 时，程序写出 DTMESH 1 文本，包含顶点 ID、XYZ 和显式三角形索引。打开时会重建 Delaunay 并逐面校验，适合研究、交换和人工检查。")
    m.h2("8.2 DTIN 二进制")
    m.para("保存扩展名为 .dtin 时使用紧凑二进制格式。DTIN v1 保存点集和 ID，打开时重新构建三角网；共圆点可能选择另一条同样合法的对角线。")
    m.table(
        ["选择", "优点", "注意"],
        [
            ("DTMESH", "文本可读、显式面索引、加载时逐面校验", "体积较大，读写较慢。"),
            ("DTIN", "紧凑、读写快、适合程序保存", "v1 不承诺逐面拓扑完全相同。"),
        ],
        [1700, 4700, 2960],
        [WD_ALIGN_PARAGRAPH.CENTER, WD_ALIGN_PARAGRAPH.LEFT, WD_ALIGN_PARAGRAPH.LEFT],
    )
    m.h2("8.3 DGRID 与 DCONTOUR 文本")
    m.para("通过“数据交换”菜单可独立导入或导出 DGRID 规则高程节点文本和 DCONTOUR 等高线文本。导入这些图层不会删除现有 TIN，因此适合叠加检查。GRID 支持完整六参数仿射变换；旋转或错切数据会按实际三个角点映射显示。安装目录的 sample_data/sample_grid.dgrid 和 sample_contours.dcontour 可直接用于验证。")
    m.callout("格式边界", "GUI 当前只接入 DGRID/DCONTOUR 文本；DLL 已提供的 GeoTIFF/COG/GeoPackage 接口将在后续菜单中开放。", "gold")
    m.h2("8.4 DCDT 约束网")
    m.para("选择“数据交换→打开约束网 DCDT”可加载基础点、外边界、孔洞、断裂线和 CRS。域内网为紫色，外边界青色，孔洞洋红，断裂线橙色。sample_constraints.dcdt 可直接验证；“保存约束网 DCDT”执行完整文本往返。")
    m.callout("独立对象", "DCDT 不替换普通 TIN，可叠加比较。查询、插入和删除按钮仍操作普通 TIN；v0.7 GUI 尚不交互编辑单条约束。", "gold")
    m.h2("8.5 往返验证")
    for step in (
        "保存当前网格。",
        "记录状态栏的顶点数和三角形数。",
        "单击“清空”。",
        "单击“打开网格”并选择刚保存的文件。",
        "比较顶点数、范围，并进行最近点和覆盖三角形查询。",
    ):
        m.number(step)

    m.h1("9 大数据量演示建议")
    m.para("DLL 对当前视口执行精确范围查询。为了避免 GDI 阻塞，二维 TIN/CDT 线框预算各约 45,000 面，三维填充与深度排序预算约 18,000 面，GRID 预览缓存最多 400 万值并缩放到不超过 512×512，等高线绘制预算约 20 万顶点。显示抽样不会删减 DLL 数据或文件导出内容。")
    m.h2("9.1 百万级点操作顺序")
    for text in (
        "先使用 10 万点确认功能和显示环境，再切换到 100 万点。",
        "生成或导入期间耐心等待，不要连续点击按钮。",
        "先框选放大到局部，再进行查询或动态编辑。",
        "避免频繁全图复位；局部范围能显示更多真实三角形细节。",
        "三维全图展示优先使用默认抽样；需要生产级连续千万点渲染时应采用 GPU 分块 LOD。",
        "超过 400 万节点的 GRID 可以保留和导出，但 GUI 不生成着色缓存或等高线。",
        "保存超大 DTMESH 前确认磁盘空间；需要紧凑存储时选 DTIN。",
    ):
        m.bullet(text)
    m.h2("9.2 16 GB 内存参考")
    m.para("项目实测 1,000 万随机均匀点约生成 2,000 万个有限三角形，峰值工作集约 4.55 GB，建网约 50.44 秒。实际数据分布、硬件、文件解析和同时运行的软件会影响结果，应预留额外内存。")

    m.h1("10 常见问题与恢复")
    faq = [
        ("程序启动失败", "确认 EXE、DLL、GMP 和 winpthread 四个文件位于同一 bin 目录。"),
        ("显示很稀疏", "全图抽样属于渲染策略；框选放大局部后会显示更完整细节。"),
        ("左键没有查询", "检查当前是否处于框选、插入、删除或三维视图。"),
        ("三维地形太平", "单击“高程×n”或按 + 提高垂直夸张。"),
        ("三维视点迷失", "按 Home 或单击“全图”恢复适屏相机。"),
        ("图层菜单有勾但画面没有", "勾表示显示开关；还需先生成或导入对应 GRID/等高线数据。"),
        ("编辑后 GRID 消失", "GRID/等高线是旧 TIN 的派生结果；TIN 改变后程序自动释放，需重新生成。"),
        ("GRID→TIN 提示跨 NoData", "普通 TIN 会跨空白构网；真实孔洞和硬边界请使用独立 DCDT 约束网。"),
        ("DCDT 孔洞仍有线框", "确认显示的是紫色 CDT 域内网；底下叠加的普通 TIN 可通过图层菜单隐藏。"),
        ("无法在画布删除约束", "v0.7 GUI 只打开、保存和显隐 DCDT；单约束编辑需调用 DLL API。"),
        ("框选后没有变化", "选框宽或高可能小于 8 像素；重新拖出更大矩形。"),
        ("无法退出框选", "单击“查询模式”等其他模式；拖动中可按 Esc。"),
        ("导入后旧网消失", "成功导入会整体替换当前网，这是设计行为；先保存需要保留的网格。"),
        ("删除的不是点击点", "删除模式按 XY 删除最近的现有网格顶点。"),
        ("文本网格被拒绝", "DTMESH 三角形必须与其顶点重建出的 Delaunay 拓扑一致。"),
    ]
    m.table(["现象", "处理方法"], faq, [2800, 6560],
            [WD_ALIGN_PARAGRAPH.LEFT, WD_ALIGN_PARAGRAPH.LEFT])

    m.h1("11 演示前检查清单")
    for text in (
        "确认 bin 中四个运行文件齐全。",
        "预先打开一次示例 XYZ 并保存 DTMESH，验证文件对话框和目录权限。",
        "根据电脑性能选择 10 万或 100 万模拟数据。",
        "演示查询前切换到查询模式，演示编辑前明确红/黄/绿含义。",
        "使用框选放大展示局部细节，并用“全图”恢复。",
        "依次生成 GRID 和等高线，用“图层”菜单验证 T/G/C 叠加与显隐。",
        "至少导出一次 DGRID 和 DCONTOUR，并重新导入检查范围和数量。",
        "打开 sample_constraints.dcdt，隐藏普通 TIN 后确认孔洞为空、三类约束颜色正确。",
        "切换 3D，验证环视、滚轮缩放、WASD 漫游和垂直夸张，再返回 2D。",
        "准备原始 XYZ 备份，清空和导入会改变当前内存网格。",
    ):
        m.bullet(text)
    m.callout("推荐演示路线", "导入示例 XYZ → 查询与编辑 → TIN→GRID → GRID 等高线 → 打开示例 DCDT → T/G/C/D 显隐 → 3D 漫游 → 分别保存四类文本。", "blue")
    m.h2("11.1 后续 GUI 升级")
    m.para("当前已交付轻量三维 TIN 查看、TIN/GRID/等高线转换和 DCDT 约束图层。后续将增加单约束交互编辑与影响显示、CDT→GRID/等高线、图层树与样式面板、GeoTIFF/GeoPackage 菜单、GPU 分块 LOD、拾取测量和贴地碰撞。")

    path = OUTPUT / "dterrain_GUI操作入门教程.docx"
    m.save(path)
    return path


if __name__ == "__main__":
    generated = [build_developer_manual(), build_gui_manual()]
    for item in generated:
        print(item)
