// Included inside throwdown_menu's private namespace. The in-game HUD of a plugin's running throwdown:
// its kit Hud document (throwdown_kit.h) drawn with SmokeyDawg58's native widgets, slot for slot.
// Nothing here knows a mode: every text, value and colour comes from the document, and the widget
// family from its theme (UI/ReSkate/Throwdowns/HUD_<theme>_*).
constexpr std::int32_t mode_hud_id=0x4d4f4400;
constexpr const char* label_widget_path="UI/Foundations/Components/Text/Label/Widget/Label_Widget";
constexpr const char* list_widget_path="UI/Foundations/Components/Lists/LinearList/Widget/LinearList_Widget";
constexpr const char* texture_widget_path="UI/Foundations/Components/Media/Icons/Texture_Widget";
constexpr const char* notice_record="TD_Page_ContentResources/TD_Notice_MenuDescriptionTitle";
constexpr const char* header_label_record="TD_Hud_ContentResources/TD_HUD_Header_Label";
constexpr const char* neon_outline="UI/Foundations/Fonts/Styles/Dingo_2pxOutline_Aqua_FontEffect";
constexpr const char* red_glow="UI/Foundations/Fonts/Styles/Dingo_3pxDropShadow_Negative_FontEffect";
constexpr const char* clean_outline="UI/Foundations/Fonts/Styles/Dingo_2pxOutline_Black_MediumDropShadow_25_FontEffect";

Address hud_art(const Context& c,const std::string& theme,const std::string& part) {
    return asset(c,("UI/ReSkate/Throwdowns/HUD_"+theme+"_"+part+"_Widget").c_str());
}
// One piece of the HUD at a screen fraction (x, y) plus pixel offsets, pinned by its left (pivot 0),
// centre (.5) or right (1) edge horizontally and its top (0) or bottom (1).
void mode_card_place(const Context& c,Value anchor,std::array<float,2> size,float x,float pivot_x,float dx,float y,float pivot_y,float dy) {
    c.set_if_changed(c.field(anchor,0x1cff7243),size);
    const auto horizontal=c.field(anchor,0xde7d30c7),vertical=c.field(anchor,0x185a5736);
    for(const auto key:{0x99bbed5aU,0x49943bedU})c.set_if_changed(c.field(horizontal,key),x);
    c.set_if_changed(c.field(horizontal,0xfec5e0b1),pivot_x);
    for(const auto key:{0x40bfbff4U,0x1d233266U})c.set_if_changed(c.field(horizontal,key),dx);
    for(const auto key:{0x99bbed5aU,0x49943bedU})c.set_if_changed(c.field(vertical,key),y);
    c.set_if_changed(c.field(vertical,0xfec5e0b1),pivot_y);
    for(const auto key:{0x40bfbff4U,0x1d233266U})c.set_if_changed(c.field(vertical,key),dy);
}
// Explicit translation for editor/intro rectangles: opposing insets must
// never shrink a fixed native asset when moving it inside its parent.
void mode_card_translate(const Context& c,Value anchor,float dx,float dy) {
    for(const auto [axis,amount]:{std::pair{0xde7d30c7U,dx},std::pair{0x185a5736U,dy}}) {
        const auto offsets=native_hud_translation(amount);
        c.set_if_changed(c.path(anchor,{axis,0x40bfbff4U}),offsets[0]);
        c.set_if_changed(c.path(anchor,{axis,0x1d233266U}),offsets[1]);
    }
}
// A private copy of one of skate.'s white text styles with one of its font effects (the aqua neon
// outline, a red glow, a black outline and shadow) and its colour scaled (red, green, blue). Any
// failure leaves plain white text rather than costing the HUD its frame.
Value custom_text_style(const Context& c,Value& style,const char* base,const char* effect,std::array<float,3> rgb={1.f,1.f,1.f}) {
    static bool failed{};
    if(failed)return {};
    if(!style.handle) try {
        const auto copy=record_model(c,base);
        if(effect)if(const auto found=asset(c,effect))c.set(c.field(copy,0x51332fc1),found);
        const std::array<std::uint32_t,3> channels{0x6fb7b2a4,0x3b385eb0,0xd1cdb49a};
        for(unsigned i=0;i<3;++i)c.set(c.path(copy,{0xc7a2ba80,0x2358a817,0xd32f6895,channels[i]}),rgb[i]);
        style=copy;
    } catch(const std::exception& e) {
        failed=true;
        logging::log(logging::Level::warning,logging::Channel::ui,"Throwdown HUD text styles unavailable, shown plain: {}",e.what());
        return {};
    }
    return style;
}
// A document colour (0xAABBGGRR) as a style's channel scale.
std::array<float,3> document_rgb(std::uint32_t color) noexcept {
    return {float(color&0xff)/255.f,float((color>>8)&0xff)/255.f,float((color>>16)&0xff)/255.f};
}
Value tinted_text_style(const Context& c,std::uint32_t color,const char* effect) {
    auto& h=state().kit_hud;
    return custom_text_style(c,h.tints[{color,effect?effect:""}],"TextStylesList/H52-XBold_White",effect,document_rgb(color));
}
void set_label_style(const Context& c,Value label,Value style) {
    for(const auto key:{0xf94d8cc6U,0xa1e84eb1U,0xfc23a999U,0x25189231U})c.set_if_changed(c.field(label,key),Ref{0,style.handle});
}
// A native label in one of skate.'s text styles; align 0 left, 2 centred.
void mode_card_label(const Context& c,Value& label,const std::string& text,const char* style,int align,Value custom={}) {
    auto& h=state().kit_hud;
    if(!label.handle) {label=make(c,label_schema);c.copy(label,c.address(h.title));}
    plain_label(c,label,text);
    if(custom.handle)set_label_style(c,label,custom);else label_styles(c,label,style,style);
    c.set_if_changed(c.field(label,0xc13ba637),false);c.set_if_changed(c.field(label,0xfc53d427),false);
    c.set_if_changed(c.field(label,0x3d8639d0),align);c.set_if_changed(c.field(label,0x87ad624c),2);
}
// Labels are shown inside a native linear list mounted on the HUD (a label mounted on its own is
// never drawn). The list is built once per set of labels.
void mode_card_list(const Context& c,Value& list,std::span<const Value> labels,float row,float width,
                    Value& anchor,Value& item,int id) {
    const auto label_widget=asset(c,label_widget_path);
    if(!list.handle || !hud_list_has_items(c,list,static_cast<unsigned>(labels.size()))) {
        std::vector<Widget> cells;
        for(const auto label:labels)cells.push_back({label_widget,{0,label.handle}});
        hud_list(c,list,cells,false,row,4.f,0.f,width);
    }
    mount_hud(c,list,asset(c,list_widget_path),anchor,item,id,false);
}
void mode_hud_cell(const Context& c,Value value,const std::string& text,float width,float height,const char* font,Value style={}) {
    match_panel_table_style(c,value);
    c.set_if_changed(c.field(value,0x13be0d51),std::array<float,2>{width,height});
    // The donor notice carries its own label constraint. Retaining it clips
    // narrow position cells even when the surrounding frame is wide enough.
    match_panel_notice_width(c,value,std::max(1.f,width-24.f));
    c.set_if_changed(c.field(value,0x54258200),false);
    const auto label=c.field(value,0x94703efc);plain_label(c,label,text);
    if(style.handle)set_label_style(c,label,style);
    else label_styles(c,label,font,font);
    c.set_if_changed(c.field(label,0xc13ba637),false);c.set_if_changed(c.field(label,0xfc53d427),false);
    c.set_if_changed(c.field(label,0x3d8639d0),2);c.set_if_changed(c.field(label,0x87ad624c),2);
}
void mode_hud_list_cell_size(const Context& c,Value list,unsigned i,float width,float height,Widget display={}) {
    const auto handle=read<Handle>(c.address(c.element(c.field(list,0xf2c90867),i)));
    const Value anchor{handle,c.type_of(handle)};
    c.set_if_changed(c.field(anchor,0x1cff7243),std::array<float,2>{width,height});
    // Weight 1 retains the explicit measured bounds. Zero collapsed the list
    // cells and separated the rendered frames from their labels.
    for(const auto axis:{0xde7d30c7U,0x185a5736U})c.set_if_changed(c.path(anchor,{axis,0xd01f4a21}),1.f);
    c.set_if_changed(c.field(anchor,0x6efc1a61),false);
    c.set_if_changed(c.field(anchor,0xbe5683d9),false);
    if(display.blueprint) {
        const auto slot=c.field(anchor,widget);const auto old=read<Widget>(c.address(slot));
        if(old.blueprint!=display.blueprint || old.data.handle!=display.data.handle)c.set_if_changed(slot,display);
    }
}
// The title frame (HUD_<theme>_Title_<w>) with the mode's logo and title, and an optional status
// line under them. `offset` moves the group inside its frame (x, y pixels).
void challenge_header(const Context& c,ChallengeHeader& h,const std::string& theme,const std::string& name,
        const std::string& logo,Value status,std::int32_t frame_id,std::int32_t content_id,std::array<float,2> offset,
        unsigned icon_size=0,unsigned frame_width=0) {
    const auto size=native_hud_header_geometry(name,icon_size,frame_width);
    const auto list_widget=asset(c,list_widget_path);
    const auto label_widget=asset(c,label_widget_path);
    const auto texture_widget=asset(c,texture_widget_path);
    if(!h.frame.handle)h.frame=record_model(c,notice_record,notice_schema);
    mode_hud_cell(c,h.frame,"",float(size.frame_width),float(size.height),"TextStylesList/D78-Caps-Bold_Bright");
    const auto frame_art=hud_art(c,theme,"Title_"+std::to_string(size.frame_width));
    mount_hud(c,h.frame,frame_art,h.frame_anchor,h.frame_item,frame_id,false);
    c.set_if_changed(c.field(h.frame_anchor,widget),Widget{frame_art,{0,h.frame.handle}});
    plain_label(c,h.title,native_hud_title_text(name));label_styles(c,h.title,"TextStylesList/D78-Caps-Bold_Bright","TextStylesList/D78-Caps-Bold_Bright");
    c.set_if_changed(c.field(h.title,0x3d8639d0),0);c.set_if_changed(c.field(h.title,0x87ad624c),2);
    c.set_if_changed(c.field(h.title,0xc13ba637),false);c.set_if_changed(c.field(h.title,0xfc53d427),false);
    // Use the native texture view's measured dimensions. Private static logo
    // widgets carry a 144px image and ignore an 80px surrounding anchor.
    c.set_if_changed(c.field(h.badge,0xf524c836),float(size.icon));c.set_if_changed(c.field(h.badge,0x6fbd254e),float(size.icon));
    c.set_if_changed(c.field(h.badge,0xd1997d0c),false);
    c.set_if_changed(c.field(h.badge,0xcd4a53c0),record_ref(c,"InteractivityColorStylesList/InteractiveColors.Core.AllWhite"));
    mode_art(c,std::array{c.field(h.badge,0x6e469d2a)},logo);
    if(!hud_list_has_items(c,h.row,2))hud_list(c,h.row,{{texture_widget,{0,h.badge.handle}},{label_widget,{0,h.title.handle}}},true,float(size.content_height),float(size.gap));
    mode_hud_list_cell_size(c,h.row,0,float(size.icon),float(size.icon),{texture_widget,{0,h.badge.handle}});
    const auto badge_cell_handle=read<Handle>(c.address(c.element(c.field(h.row,0xf2c90867),0)));
    const Value badge_cell{badge_cell_handle,c.type_of(badge_cell_handle)};
    const auto badge_offset=native_hud_translation(float(size.content_height-size.icon)*.5f+size.badge_y_shift);
    c.set_if_changed(c.path(badge_cell,{0x185a5736U,0x40bfbff4U}),badge_offset[0]);
    c.set_if_changed(c.path(badge_cell,{0x185a5736U,0x1d233266U}),badge_offset[1]);
    mode_hud_list_cell_size(c,h.row,1,float(size.title_width),float(size.text_height),{label_widget,{0,h.title.handle}});
    const auto title_cell_handle=read<Handle>(c.address(c.element(c.field(h.row,0xf2c90867),1)));
    const Value title_cell{title_cell_handle,c.type_of(title_cell_handle)};
    const auto title_offset=native_hud_translation(size.title_y_shift);
    c.set_if_changed(c.path(title_cell,{0x185a5736U,0x40bfbff4U}),title_offset[0]);
    c.set_if_changed(c.path(title_cell,{0x185a5736U,0x1d233266U}),title_offset[1]);
    const unsigned count=status.handle?2:1;
    if(!hud_list_has_items(c,h.content,count)) {
        std::vector<Widget> rows{{list_widget,{0,h.row.handle}}};if(status.handle)rows.push_back({label_widget,{0,status.handle}});
        hud_list(c,h.content,rows,false,float(size.content_height),8.f,0.f,float(size.content_width));
    }
    mode_hud_list_cell_size(c,h.content,0,float(size.content_width),float(size.content_height));
    if(status.handle)mode_hud_list_cell_size(c,h.content,1,float(size.content_width),48.f);
    mount_hud(c,h.content,list_widget,h.anchor,h.item,content_id,false);
    for(const auto anchor:{h.frame_anchor,h.anchor}) {
        const bool frame=anchor.handle==h.frame_anchor.handle;
        c.set_if_changed(c.field(anchor,0x1cff7243),std::array<float,2>{float(frame?size.frame_width:size.content_width),frame?float(size.height):status.handle?float(size.content_height)+56.f:float(size.content_height)});
        for(const auto axis:{0xde7d30c7U,0x185a5736U}) {
            const bool vertical=axis==0x185a5736U;
            for(const auto key:{0x99bbed5aU,0x49943bedU})c.set_if_changed(c.path(anchor,{axis,key}),vertical?native_hud_screen_anchor(.008f):.5f);
            c.set_if_changed(c.path(anchor,{axis,0xfec5e0b1}),vertical?0.f:.5f);
            c.set_if_changed(c.path(anchor,{axis,0xd01f4a21}),1.f);
            const float move=vertical?(frame?0.f:float(size.height-size.content_height)*.5f)+offset[1]:offset[0];
            const auto offsets=native_hud_translation(move);
            c.set_if_changed(c.path(anchor,{axis,0x40bfbff4U}),offsets[0]);
            c.set_if_changed(c.path(anchor,{axis,0x1d233266U}),offsets[1]);
        }
    }
}
Address mode_hud_caption_art(const Context& c,float width) {
    return asset(c,("UI/ReSkate/Throwdowns/Caption_"+std::to_string(static_cast<unsigned>(width))+"_Widget").c_str());
}
Address mode_hud_heading_art(const Context& c,float width,unsigned height) {
    return asset(c,("UI/ReSkate/Throwdowns/Heading_"+std::to_string(static_cast<unsigned>(width))+"_"+std::to_string(height)+"_Widget").c_str());
}
void mode_board_columns(const Context& c,Value& list,std::array<Value,3>& labels,const std::array<float,3>& widths,
                        const std::array<std::string,3>& captions) {
    std::vector<Widget> cells;
    for(unsigned i=0;i<3;++i) {
        if(!labels[i].handle)labels[i]=record_model(c,notice_record,notice_schema);
        mode_hud_cell(c,labels[i],captions[i],widths[i],52.f,"TextStylesList/H52-XBold_White");
        cells.push_back({mode_hud_caption_art(c,widths[i]),{0,labels[i].handle}});
    }
    if(!hud_list_has_items(c,list,3))hud_list(c,list,cells,true,52.f,8.f);
    for(unsigned i=0;i<3;++i)mode_hud_list_cell_size(c,list,i,widths[i],52.f,cells[i]);
}
std::array<std::string,3> board_captions(const std::vector<std::string>& columns,const std::string& value={}) {
    std::array<std::string,3> result{"POS","NAME","SCORE"};
    for(std::size_t i=0;i<columns.size() && i<3;++i)result[i]=columns[i];
    if(!value.empty())result[2]=value;
    return result;
}
std::uint64_t clock_now(const kit::Clock& clock,std::uint64_t now) {
    return clock.up?native_hud_clock_elapsed(clock.remaining_ms,clock.sampled_ms,now,clock.ticking,clock.total_ms):
        native_hud_clock_remaining(clock.remaining_ms,clock.sampled_ms,now,clock.ticking);
}
// A row's value cell: its value, a live clock after it (the value is then a prefix, "DNF  "), or
// its marks (the ones that are on, spaced; none yet, a dash).
std::string board_value(const kit::BoardRow& row,std::uint64_t now=GetTickCount64()) {
    if(row.clock.present)return row.value+native_hud_clock_text(clock_now(row.clock,now));
    if(row.marks.empty())return row.value;
    std::string spelled;
    for(const auto& mark:row.marks)if(mark.on)spelled+=(spelled.empty()?"":" ")+mark.text;
    return spelled.empty()?std::string("-"):spelled;
}
bool board_marked(const kit::BoardRow& row) {
    return std::any_of(row.marks.begin(),row.marks.end(),[](const kit::Mark& mark){return mark.on;});
}
// The widest a row's value gets: a running clock's at its cap (and the board's reserve text, such
// as "DNF  ", before it), reserved before the clock reaches it.
unsigned board_value_reserve(const kit::BoardRow& row,const std::string& reserve) {
    auto width=native_hud_value_width(board_value(row));
    if(row.clock.present && row.clock.total_ms) {
        const auto cap=native_hud_clock_text(row.clock.total_ms);
        width=std::max(width,native_hud_value_width(row.value+cap));
        if(!reserve.empty())width=std::max(width,native_hud_value_width(reserve+cap));
    }
    return width;
}
// The widget family of a row's name cell: the board's own (an out row's when it has one), else the
// theme's.
std::string name_family(const kit::BoardRow& row,const std::string& theme,const std::string& name_art,const std::string& out_art) {
    if(row.out && !out_art.empty())return out_art;
    if(!name_art.empty())return name_art;
    return "HUD_"+theme+"_Name";
}
// The board's rows, into h.board_rows: POS / NAME / value cells in the board's widget families (the
// theme's by default), or (variant "stamps") a name and a stamp per mark. Outlined boards set their
// cells in outlined text; a row's colour tints its value.
float kit_board_rows(const Context& c,KitHud& h,const std::vector<kit::BoardRow>& rows,const std::string& theme,
                     const kit::Hud::Board& board,bool styled) {
    const bool stamps=board.variant=="stamps";
    const auto& name_art=board.name_art;
    const auto& out_art=board.out_art;
    const auto list_widget=asset(c,list_widget_path);
    const auto count=std::min<std::size_t>(rows.size(),6);
    float name_width=224.f,value_width=board.value_width>0.f?board.value_width:224.f;
    std::size_t most_marks{};
    for(std::size_t i=0;i<count;++i) {
        name_width=std::max(name_width,float(native_hud_name_width(rows[i].name)));
        value_width=std::max(value_width,float(board_value_reserve(rows[i],board.reserve)));
        most_marks=std::max(most_marks,rows[i].marks.size());
    }
    h.widths={100.f,name_width,value_width};
    constexpr float height=76.f;
    const float width=stamps?std::max(496.f,name_width+16.f+84.f*float(most_marks)):116.f+name_width+value_width;
    std::vector<Widget> refs;
    for(std::size_t i=0;i<count;++i) {
        const auto& source=rows[i];auto& row=h.rows[i];
        const auto key=stamps?"stamps:"+std::to_string(source.marks.size()):std::string("cells");
        if(row.key!=key) {row.list={};row.key=key;}
        std::vector<Widget> cells;
        if(stamps) {
            const float own=float(native_hud_name_width(source.name));
            if(!row.name.handle) {
                row.name=record_model(c,notice_record,notice_schema);
                const auto label=c.field(row.name,0x94703efc);
                c.set_if_changed(c.field(label,0x87ad624c),2);c.set_if_changed(c.field(label,0x3d8639d0),2);
                c.set_if_changed(c.field(label,0xc13ba637),false);c.set_if_changed(c.field(label,0xfc53d427),false);
            }
            c.set_if_changed(c.field(row.name,0x13be0d51),std::array<float,2>{own,height});
            const auto label=c.field(row.name,0x94703efc);plain_label(c,label,source.name);
            const auto font=source.out?"TextStylesList/H52-XBold_Grey500":"TextStylesList/H52-XBold_White";
            label_styles(c,label,font,font);
            cells.push_back({asset(c,("UI/ReSkate/Throwdowns/"+name_family(source,theme,name_art,out_art)+"_"+std::to_string(static_cast<unsigned>(own))+"_Widget").c_str()),{0,row.name.handle}});
            row.stamps.resize(source.marks.size());
            for(std::size_t m=0;m<source.marks.size();++m) {
                auto& stamp=row.stamps[m];
                if(!stamp.handle) {
                    stamp=record_model(c,"Activities_HudElementData_ContentResource/Activity_HUD_CurrentScore_Notice");
                    c.set_if_changed(c.field(stamp,0x13be0d51),std::array<float,2>{height,height});
                    const auto mark=c.field(stamp,0x94703efc);
                    label_styles(c,mark,"TextStylesList/H52-XBold_White","TextStylesList/H52-XBold_White");
                    c.set_if_changed(c.field(mark,0x87ad624c),2);c.set_if_changed(c.field(mark,0x3d8639d0),2);
                    c.set_if_changed(c.field(mark,0xc13ba637),false);
                }
                plain_label(c,c.field(stamp,0x94703efc),source.marks[m].text);
                match_panel_notice_width(c,stamp,height-32.f);
                const auto style=record_ref(c,source.marks[m].on?"NoticeStylesList/SmallNotice_ControlSelection_Focus":
                    "NoticeStylesList/SmallNotice_ControlSelection_Default");
                c.set_if_changed(c.field(stamp,0x8cc042ef),style);c.set_if_changed(c.field(stamp,0xa1e84eb1),style);
                cells.push_back({asset(c,"UI/Foundations/Components/Notices/SmallNotice_Widget"),{0,stamp.handle}});
            }
            if(!hud_list_has_items(c,row.list,static_cast<unsigned>(cells.size())))
                hud_list(c,row.list,cells,true,height,8.f,0.f,width,own);
            for(unsigned col=0;col<cells.size();++col) {
                mode_hud_list_cell_size(c,row.list,col,col==0?own:height,height,cells[col]);
                const auto handle=read<Handle>(c.address(c.element(c.field(row.list,0xf2c90867),col)));
                for(const auto offset:{0x40bfbff4U,0x1d233266U})c.set_if_changed(c.path({handle,c.type_of(handle)},{0x185a5736,offset}),0.f);
            }
        } else {
            std::array<Value*,3> values{&row.rank,&row.name,&row.value};
            for(unsigned column=0;column<3;++column) {
                if(!values[column]->handle)*values[column]=record_model(c,notice_record,notice_schema);
                const std::string text=column==0?source.rank:column==1?source.name:board_value(source);
                Value style;
                if(column==2 && !source.marks.empty() && !board_marked(source))
                    style=custom_text_style(c,h.style_grey,"TextStylesList/H52-XBold_White",clean_outline,{.55f,.55f,.55f});
                else if(column==2 && source.color!=0xffffffffU)style=tinted_text_style(c,source.color,styled?red_glow:nullptr);
                else if(styled)style=custom_text_style(c,h.style_clean52,"TextStylesList/H52-XBold_White",clean_outline);
                mode_hud_cell(c,*values[column],text,h.widths[column],height,"TextStylesList/H52-XBold_White",style);
                std::string art;
                if(column==0)art="UI/ReSkate/Throwdowns/"+(board.rank_art.empty()?"HUD_"+theme+"_Rank":board.rank_art)+"_Widget";
                else if(column==1)art="UI/ReSkate/Throwdowns/"+name_family(source,theme,name_art,out_art)+"_"+std::to_string(static_cast<unsigned>(name_width))+"_Widget";
                else art="UI/ReSkate/Throwdowns/"+(board.value_art.empty()?"HUD_"+theme+"_Value":board.value_art)+"_"+std::to_string(static_cast<unsigned>(value_width))+"_Widget";
                cells.push_back({asset(c,art.c_str()),{0,values[column]->handle}});
            }
            if(!hud_list_has_items(c,row.list,3))hud_list(c,row.list,cells,true,height,8.f);
            for(unsigned col=0;col<3;++col)mode_hud_list_cell_size(c,row.list,col,h.widths[col],height,cells[col]);
        }
        refs.push_back({list_widget,{0,row.list.handle}});
    }
    std::vector<Handle> order;for(std::size_t i=0;i<count;++i)order.push_back(h.rows[i].list.handle);
    if(h.row_order!=order || !hud_list_has_items(c,h.board_rows,static_cast<unsigned>(refs.size())))
        hud_list(c,h.board_rows,refs,false,height,8.f,0.f,width);
    h.row_order=std::move(order);
    for(unsigned i=0;i<refs.size();++i)mode_hud_list_cell_size(c,h.board_rows,i,width,height);
    return width;
}
// The board: its heading, column captions and rows, at the right edge ("side") or bottom right
// ("bottom", as Skate 3's S.K.A.T.E. scoreboard).
// The "list" heading (a stamps board's): one list of a small label row, the counters and the rows,
// at the right edge.
void kit_board_list(const Context& c,const kit::Hud& hud,float width,float rows_height) {
    auto& h=state().kit_hud;
    const auto list_widget=asset(c,list_widget_path);
    if(!h.board_label.handle)h.board_label=record_model(c,header_label_record);
    label_styles(c,h.board_label,"TextStylesList/SP38-SemiB_White","TextStylesList/SP38-SemiB_White");
    plain_label(c,h.board_label,hud.board.title);
    const auto counters=std::min<std::size_t>(hud.counters.size(),h.counters.size());
    const float counters_height=counters?80.f*float(counters)-8.f:0.f;
    std::vector<Widget> children{{asset(c,label_widget_path),{0,h.board_label.handle}}};
    if(counters)children.push_back({list_widget,{0,h.score.handle}});
    children.push_back({list_widget,{0,h.board_rows.handle}});
    if(h.board_children!=children.size()+100 || !hud_list_has_items(c,h.board,static_cast<unsigned>(children.size()))) {
        hud_list(c,h.board,children,false,48.f,12.f,rows_height,width);
        h.board_children=static_cast<unsigned>(children.size())+100; // a list board's composition, apart from a shared one's
    }
    mode_hud_list_cell_size(c,h.board,0,width,48.f);
    if(counters)mode_hud_list_cell_size(c,h.board,1,width,counters_height);
    mode_hud_list_cell_size(c,h.board,counters?2:1,width,rows_height);
    mount_hud(c,h.board,list_widget,h.board_anchor,h.board_item,mode_hud_id+6,false);
    const float height=48.f+12.f+(counters?counters_height+12.f:0.f)+rows_height;
    c.set_if_changed(c.field(h.board_anchor,0x1cff7243),std::array<float,2>{width,height});
    for(const auto axis:{0xde7d30c7U,0x185a5736U}) {
        const bool vertical=axis==0x185a5736U;
        for(const auto field:{0x99bbed5aU,0x49943bedU})c.set_if_changed(c.path(h.board_anchor,{axis,field}),
            native_hud_screen_anchor(vertical?native_hud_side_center:.994f));
        for(const auto field:{0x40bfbff4U,0x1d233266U})c.set_if_changed(c.path(h.board_anchor,{axis,field}),0.f);
        c.set_if_changed(c.path(h.board_anchor,{axis,0xfec5e0b1}),vertical?.5f:1.f);
        c.set_if_changed(c.path(h.board_anchor,{axis,0xd01f4a21}),1.f);
    }
}
void kit_board(const Context& c,const kit::Hud& hud,bool styled) {
    auto& h=state().kit_hud;
    const auto list_widget=asset(c,list_widget_path);
    const bool stamps=hud.board.variant=="stamps";
    const float width=kit_board_rows(c,h,hud.board.rows,hud.theme,hud.board,styled);
    const float rows_height=84.f*static_cast<float>(std::min<std::size_t>(hud.board.rows.size(),6));
    if(hud.board.heading=="list") {kit_board_list(c,hud,width,rows_height);return;}
    if(!h.board_title.handle)h.board_title=record_model(c,notice_record,notice_schema);
    mode_hud_cell(c,h.board_title,hud.board.title,width,64.f,"TextStylesList/H52-XBold_White",
                  styled?custom_text_style(c,h.style_neon52,"TextStylesList/H52-XBold_White",neon_outline):Value{});
    const bool columns=!stamps && !hud.board.columns.empty();
    if(columns)mode_board_columns(c,h.board_columns,h.board_captions,h.widths,board_captions(hud.board.columns));
    const auto heading=mode_hud_heading_art(c,width,64);
    std::vector<Widget> children{{heading,{0,h.board_title.handle}}};
    if(columns)children.push_back({list_widget,{0,h.board_columns.handle}});
    children.push_back({list_widget,{0,h.board_rows.handle}});
    if(h.board_children!=children.size() || !hud_list_has_items(c,h.board,static_cast<unsigned>(children.size()))) {
        hud_list(c,h.board,children,false,64.f,12.f,0.f,width);
        h.board_children=static_cast<unsigned>(children.size());
    }
    mode_hud_list_cell_size(c,h.board,0,width,64.f,children[0]);
    if(columns)mode_hud_list_cell_size(c,h.board,1,width,52.f);
    mode_hud_list_cell_size(c,h.board,columns?2:1,width,rows_height);
    mount_hud(c,h.board,list_widget,h.board_anchor,h.board_item,mode_hud_id+6,false);
    c.set_if_changed(c.field(h.board_anchor,0x1cff7243),std::array<float,2>{width,(columns?140.f:76.f)+rows_height});
    const bool bottom=hud.layout=="bottom";
    for(const auto axis:{0xde7d30c7U,0x185a5736U}) {
        const bool vertical=axis==0x185a5736U;
        const float at=bottom?native_hud_screen_anchor(vertical?.95f:.97f):
            vertical?native_hud_screen_anchor(native_hud_side_center):native_hud_screen_anchor(.994f);
        for(const auto field:{0x99bbed5aU,0x49943bedU})c.set_if_changed(c.path(h.board_anchor,{axis,field}),at);
        c.set_if_changed(c.path(h.board_anchor,{axis,0xfec5e0b1}),bottom?1.f:vertical?.5f:1.f);
        c.set_if_changed(c.path(h.board_anchor,{axis,0xd01f4a21}),1.f);
        for(const auto field:{0x40bfbff4U,0x1d233266U})c.set_if_changed(c.path(h.board_anchor,{axis,field}),0.f);
    }
}
void kit_results(const Context& c,const kit::Hud& hud) {
    auto& h=state().kit_hud;const auto& r=hud.results;
    // The rows in the board's cell families; never stamps (a results table is cells).
    auto cells=hud.board;
    cells.variant.clear();
    kit_board_rows(c,h,r.rows,hud.theme,cells,r.style=="outlined");
    const auto list_widget=asset(c,list_widget_path);
    const float row_width=h.widths[0]+h.widths[1]+h.widths[2]+16.f;
    const auto panel_width=static_cast<unsigned>(std::ceil(std::max(1164.f,row_width+64.f)/32.f))*32u;
    const float inside_width=static_cast<float>(panel_width)-64.f;
    const float title_width=r.title_width>0.f?std::min(r.title_width,inside_width-168.f):std::min(inside_width-168.f,784.f);
    const float header_width=title_width+168.f;
    const auto count=std::clamp<unsigned>(static_cast<unsigned>(r.rows.size()),1,6);
    const float panel_height=432.f+84.f*count,rows_height=84.f*static_cast<float>(count);
    if(!h.results_title.handle)h.results_title=record_model(c,notice_record,notice_schema);
    mode_hud_cell(c,h.results_title,r.title,title_width,144.f,"TextStylesList/D94-Caps-Bold_White");
    c.set_if_changed(c.path(h.results_title,{0x94703efc,0x3d8639d0}),0);
    const auto heading=asset(c,("UI/ReSkate/Throwdowns/"+(r.heading_art.empty()?std::string("ResultsHeading"):r.heading_art)+"_Widget").c_str());
    const auto logo=hud_art(c,hud.theme,"Logo");
    if(!hud_list_has_items(c,h.results_header,2))hud_list(c,h.results_header,{{logo,{}},{heading,{0,h.results_title.handle}}},true,144.f,24.f);
    mode_hud_list_cell_size(c,h.results_header,0,144.f,144.f,{logo,{}});
    mode_hud_list_cell_size(c,h.results_header,1,title_width,144.f,{heading,{0,h.results_title.handle}});
    if(!h.results_notice.handle)h.results_notice=record_model(c,notice_record,notice_schema);
    mode_hud_cell(c,h.results_notice,r.notice,inside_width,96.f,"TextStylesList/H52-XBold_White");
    const auto notice=hud_art(c,hud.theme,"Results_"+std::to_string(panel_width-64));
    mode_board_columns(c,h.results_columns,h.results_captions,h.widths,board_captions(hud.board.columns,r.caption));
    if(!hud_list_has_items(c,h.results,4))hud_list(c,h.results,{{list_widget,{0,h.results_header.handle}},
        {notice,{0,h.results_notice.handle}},{list_widget,{0,h.results_columns.handle}},{list_widget,{0,h.board_rows.handle}}},false,144.f,24.f,0.f,inside_width);
    mode_hud_list_cell_size(c,h.results,0,header_width,144.f);
    const auto header_cell=read<Handle>(c.address(c.element(c.field(h.results,0xf2c90867),0)));
    // Native vertical lists already centre narrower cells. An extra half-gap
    // translates them a second time and pushes the header/table to the right.
    mode_card_translate(c,{header_cell,c.type_of(header_cell)},0.f,0.f);
    mode_hud_list_cell_size(c,h.results,1,inside_width,96.f,{notice,{0,h.results_notice.handle}});
    mode_hud_list_cell_size(c,h.results,2,row_width,52.f);
    mode_hud_list_cell_size(c,h.results,3,row_width,rows_height);
    for(unsigned i:{2u,3u}) {
        const auto cell=read<Handle>(c.address(c.element(c.field(h.results,0xf2c90867),i)));
        mode_card_translate(c,{cell,c.type_of(cell)},0.f,0.f);
    }
    const auto panel=asset(c,("UI/ReSkate/Throwdowns/ResultsPanel"+std::to_string(count)+"_"+std::to_string(panel_width)+"_Widget").c_str());
    mount_hud(c,{},panel,h.results_panel_anchor,h.results_panel_item,mode_hud_id+60,true);
    c.set_if_changed(c.field(h.results_panel_anchor,widget),Widget{panel,{}});
    mount_hud(c,h.results,list_widget,h.results_anchor,h.results_item,mode_hud_id+5,true);
    const auto place=[&](Value anchor,float width,float height,float padding) {
        c.set_if_changed(c.field(anchor,0x1cff7243),std::array<float,2>{width,height});
        for(const auto axis:{0xde7d30c7U,0x185a5736U}) {
            const bool vertical=axis==0x185a5736U;
            for(const auto field:{0x99bbed5aU,0x49943bedU})c.set_if_changed(c.path(anchor,{axis,field}),.5f);
            c.set_if_changed(c.path(anchor,{axis,0xfec5e0b1}),0.f);
            c.set_if_changed(c.path(anchor,{axis,0xd01f4a21}),1.f);
            const auto move=native_hud_translation((vertical?-panel_height*.5f:-static_cast<float>(panel_width)*.5f)+padding);
            c.set_if_changed(c.path(anchor,{axis,0x40bfbff4U}),move[0]);
            c.set_if_changed(c.path(anchor,{axis,0x1d233266U}),move[1]);
        }
    };
    place(h.results_panel_anchor,static_cast<float>(panel_width),panel_height,0.f);
    place(h.results_anchor,inside_width,panel_height-64.f,32.f);
}
// Place a top-left cluster piece (the position medal, its timers and counters): pixels from the
// safe frame's top left.
void place_top_left(const Context& c,Value anchor,float w,float height,float x,float y) {
    c.set_if_changed(c.field(anchor,0x1cff7243),std::array<float,2>{w,height});
    for(const auto axis:{0xde7d30c7U,0x185a5736U}) {
        const bool vertical=axis==0x185a5736U;
        for(const auto field:{0x99bbed5aU,0x49943bedU})c.set_if_changed(c.path(anchor,{axis,field}),vertical?native_hud_screen_anchor(.008f):native_hud_screen_anchor(.006f));
        c.set_if_changed(c.path(anchor,{axis,0xfec5e0b1}),0.f);
        c.set_if_changed(c.path(anchor,{axis,0xd01f4a21}),1.f);
        for(const auto field:{0x40bfbff4U,0x1d233266U})c.set_if_changed(c.path(anchor,{axis,field}),vertical?y:x);
    }
}
std::string timer_text(const kit::TimerText& timer,std::uint64_t now) {
    return timer.clock.present?native_hud_clock_text(clock_now(timer.clock,now)):timer.text;
}
// The first timer with a caption is the one shown captioned (one caption slot).
std::size_t captioned_timer(const std::vector<kit::TimerText>& timers) {
    for(std::size_t i=0;i<timers.size() && i<2;++i)if(!timers[i].caption.empty())return i;
    return SIZE_MAX;
}
constexpr float timer_width=384.f;
void kit_timers(const Context& c,const kit::Hud& hud,std::uint64_t now) {
    auto& h=state().kit_hud;
    const auto captioned=captioned_timer(hud.timers);
    float y=0.f;
    for(std::size_t i=0;i<hud.timers.size() && i<2;++i) {
        if(i==captioned) {
            if(!h.timer_caption.handle)h.timer_caption=record_model(c,header_label_record);
            plain_label(c,h.timer_caption,hud.timers[i].caption);
            label_styles(c,h.timer_caption,"TextStylesList/H52-XBold_White","TextStylesList/H52-XBold_White");
            c.set_if_changed(c.field(h.timer_caption,0x3d8639d0),2);
            mount_hud(c,h.timer_caption,asset(c,label_widget_path),h.timer_caption_anchor,h.timer_caption_item,mode_hud_id+63,false);
            place_top_left(c,h.timer_caption_anchor,timer_width,56.f,520.f,y);
            y+=62.f;
        }
        if(!h.timer_values[i].handle)h.timer_values[i]=record_model(c,notice_record,notice_schema);
        mode_hud_cell(c,h.timer_values[i],timer_text(hud.timers[i],now),timer_width,100.f,"TextStylesList/H64-XBold_White");
        const auto family=hud.timer_art.empty()?"HUD_"+hud.theme+"_Elapsed":hud.timer_art;
        mount_hud(c,h.timer_values[i],asset(c,("UI/ReSkate/Throwdowns/"+family+"_Widget").c_str()),h.timer_anchors[i],h.timer_items[i],mode_hud_id+61+static_cast<int>(i),false);
        place_top_left(c,h.timer_anchors[i],timer_width,100.f,520.f,y);
        y+=110.f;
    }
}
// Between full HUD ticks only text advances: a running clock's seconds and timers' times.
void kit_hud_clocks(const Context& c,const kit::Hud& hud,std::uint64_t now) {
    auto& h=state().kit_hud;
    for(std::size_t i=0;i<hud.timers.size() && i<2;++i)
        if(hud.timers[i].clock.present && h.timer_values[i].handle && c.type_of(h.timer_values[i].handle)==h.timer_values[i].type)
            plain_label(c,c.field(h.timer_values[i],0x94703efc),timer_text(hud.timers[i],now));
    if(hud.clock.present && h.clock.handle && c.type_of(h.clock.handle)==h.clock.type)
        native_timer_seconds(c,h.clock,0x0ec4a8c0,clock_now(hud.clock,now));
    const auto& rows=hud.results.present?hud.results.rows:hud.board.rows;
    for(std::size_t i=0;i<rows.size() && i<6;++i) {
        if(!rows[i].clock.present)continue;
        const auto found=h.rows.find(i);
        if(found!=h.rows.end() && found->second.value.handle && c.type_of(found->second.value.handle)==found->second.value.type)
            plain_label(c,c.field(found->second.value,0x94703efc),board_value(rows[i],now));
    }
}
bool kit_hud_ticks(const kit::Hud& hud) {
    if(hud.compass.present || (hud.clock.present && hud.clock.ticking))return true;
    const auto& rows=hud.results.present?hud.results.rows:hud.board.rows;
    if(std::any_of(rows.begin(),rows.end(),[](const kit::BoardRow& row){return row.clock.present && row.clock.ticking;}))return true;
    return std::any_of(hud.timers.begin(),hud.timers.end(),[](const kit::TimerText& timer){return timer.clock.present && timer.clock.ticking;});
}
void kit_position(const Context& c,const kit::Hud& hud) {
    auto& h=state().kit_hud;
    if(!h.position.handle)h.position=record_model(c,notice_record,notice_schema);
    const auto place=hud.position.place;
    const auto suffix=place%100>=11 && place%100<=13?"TH":place%10==1?"ST":place%10==2?"ND":place%10==3?"RD":"TH";
    mode_hud_cell(c,h.position,place?std::to_string(place)+suffix:std::string("--"),480.f,300.f,"TextStylesList/D94-Caps-Bold_White");
    c.set_if_changed(c.path(h.position,{0x94703efc,0x3d8639d0}),2);
    auto medal=hud.position.medal.empty()?std::string("white"):hud.position.medal;
    medal[0]=static_cast<char>(std::toupper(static_cast<unsigned char>(medal[0])));
    const auto family=hud.position.art.empty()?"HUD_"+hud.theme+"_Position":hud.position.art;
    const auto art=asset(c,("UI/ReSkate/Throwdowns/"+family+"_"+medal+"_Widget").c_str());
    mount_hud(c,h.position,art,h.position_anchor,h.position_item,mode_hud_id+41,false);
    c.set_if_changed(c.field(h.position_anchor,widget),Widget{art,{0,h.position.handle}});
    place_top_left(c,h.position_anchor,480.f,300.f,24.f,0.f);
}
// The counters: the score block above the board, or, beside a position medal, a column under it.
// A board with the "list" heading holds the counters itself (kit_board): `mount` false builds them only.
void kit_counters(const Context& c,const kit::Hud& hud,bool mount=true) {
    auto& h=state().kit_hud;
    const auto list_widget=asset(c,list_widget_path);
    const auto count=std::min<std::size_t>(hud.counters.size(),h.counters.size());
    const bool left=hud.position.present;
    const float width=left?480.f:hud.score_width;
    for(std::size_t i=0;i<count;++i) {
        auto& value=h.counters[i];
        if(!value.handle)value=record_model(c,notice_record,notice_schema);
        c.set_if_changed(c.field(value,0x13be0d51),std::array<float,2>{width,72.f});
        // The stock 480 strip keeps its notice's own label constraint, as his score block had it.
        if(width!=480.f)match_panel_notice_width(c,value,width-24.f);
        const auto label=c.field(value,0x94703efc);
        label_styles(c,label,"TextStylesList/H52-XBold_White","TextStylesList/H52-XBold_White");
        c.set_if_changed(c.field(label,0x3d8639d0),0);c.set_if_changed(c.field(label,0x87ad624c),2);c.set_if_changed(c.field(label,0xc13ba637),false);
        plain_label(c,label,hud.counters[i]);
    }
    const auto key=hud.theme+":"+hud.score_art+":"+std::to_string(count)+":"+std::to_string(width)+(left?":left":"");
    if(h.score_key!=key || !hud_list_has_items(c,h.score,static_cast<unsigned>(count))) {
        const auto art=hud.score_art.empty()?hud_art(c,hud.theme,"Score"):asset(c,("UI/ReSkate/Throwdowns/"+hud.score_art+"_Widget").c_str());
        std::vector<Widget> cells;
        for(std::size_t i=0;i<count;++i)cells.push_back({art,{0,h.counters[i].handle}});
        hud_list(c,h.score,cells,false,72.f,8.f,0.f,width);h.score_key=key;
    }
    if(!mount)return;
    const float height=80.f*float(count)-8.f;
    if(left) {
        mount_hud(c,h.score,list_widget,h.score_anchor,h.score_item,mode_hud_id+46,false);
        place_top_left(c,h.score_anchor,width,height,24.f,324.f);
        return;
    }
    mount_hud(c,h.score,list_widget,h.score_anchor,h.score_item,mode_hud_id+59,false);
    c.set_if_changed(c.field(h.score_anchor,0x1cff7243),std::array<float,2>{width,height});
    const auto rows=static_cast<float>(std::min<std::size_t>(6,hud.board.rows.size()));
    for(const auto axis:{0xde7d30c7U,0x185a5736U}) {
        const bool y=axis==0x185a5736U;
        for(const auto key_field:{0x99bbed5aU,0x49943bedU})c.set_if_changed(c.path(h.score_anchor,{axis,key_field}),y?native_hud_screen_anchor(native_hud_side_center):native_hud_screen_anchor(.994f));
        c.set_if_changed(c.path(h.score_anchor,{axis,0xfec5e0b1}),1.f);
        for(const auto key_field:{0x40bfbff4U,0x1d233266U})c.set_if_changed(c.path(h.score_anchor,{axis,key_field}),y?-(140.f+84.f*rows)*.5f-24.f:0.f);
        c.set_if_changed(c.path(h.score_anchor,{axis,0xd01f4a21}),1.f);
    }
}
void kit_clock(const Context& c,const kit::Hud& hud,std::uint64_t now) {
    auto& h=state().kit_hud;
    if(!h.clock.handle)h.clock=record_model(c,"Activities_HudElementData_ContentResource/A_ActivityCountdown");
    mount_hud(c,h.clock,asset(c,"UI/Foundations/Components/Countdown/Countdown_Widget"),h.clock_anchor,h.clock_item,mode_hud_id+1,false);
    c.set_if_changed(c.field(h.clock_anchor,0x1cff7243),std::array<float,2>{320.f,90.f});
    for(const auto axis:{0xde7d30c7U,0x185a5736U}) {
        const auto layout=c.field(h.clock_anchor,axis);
        const bool vertical=axis==0x185a5736U;
        for(const auto key:{0x99bbed5aU,0x49943bedU})c.set_if_changed(c.field(layout,key),vertical?native_hud_screen_anchor(.988f):.5f);
        c.set_if_changed(c.field(layout,0xfec5e0b1),vertical?1.f:.5f);
        c.set_if_changed(c.field(layout,0xd01f4a21),1.f);
        for(const auto key:{0x40bfbff4U,0x1d233266U})c.set_if_changed(c.field(layout,key),0.f);
    }
    // CountdownViewModel with DoCountdown off shows the time it is given.
    c.set_if_changed(c.field(h.clock,0x703c4d93),false);c.set_if_changed(c.field(h.clock,0x167131bb),true);
    c.set_if_changed(c.field(h.clock,0x6186003c),true);c.set_if_changed(c.field(h.clock,0xc11082ee),true);
    native_timer_seconds(c,h.clock,0x4cc5ec89,hud.clock.total_ms);
    native_timer_seconds(c,h.clock,0x0ec4a8c0,clock_now(hud.clock,now));
}
void kit_warning(const Context& c,const kit::Hud& hud) {
    auto& h=state().kit_hud;
    if(!h.warning.handle)h.warning=record_model(c,notice_record,notice_schema);
    for(const auto key:{0x8cc042efU,0xa1e84eb1U})c.set_if_changed(c.field(h.warning,key),record_ref(c,"NoticeStylesList/BasicNotice_Alert"));
    const auto label=c.field(h.warning,0x94703efc);
    plain_label(c,label,hud.warning.text);
    label_styles(c,label,"TextStylesList/H52-XBold_White","TextStylesList/H52-XBold_White");
    c.set_if_changed(c.field(h.warning,0x13be0d51),std::array<float,2>{1000.f,96.f});
    const auto family=hud.warning.art.empty()?"HUD_"+hud.theme+"_Warning":hud.warning.art;
    mount_hud(c,h.warning,asset(c,("UI/ReSkate/Throwdowns/"+family+"_Widget").c_str()),h.warning_anchor,h.warning_item,mode_hud_id+42,true);
    c.set_if_changed(c.field(h.warning_anchor,0x1cff7243),std::array<float,2>{1000.f,96.f});
    for(const auto field:{0x99bbed5aU,0x49943bedU})c.set_if_changed(c.path(h.warning_anchor,{0x185a5736,field}),.3f);
}
// The native Throwdown banner across the screen: its title, subtitle and art (the header's logo
// when it has none).
void kit_banner(const Context& c,const kit::Hud& hud) {
    auto& h=state().kit_hud;
    if(!h.banner.handle) {
        h.banner=record_model(c,"TD_Hud_ContentResources/TD_ActivityBanner");
        c.set_if_changed(c.field(h.banner,0xf79313be),record_ref(c,"ActivityBannerStylesList/ActivityBannerCenterFullWidthStyle.Throwdown.Default"));
        const auto heading=c.field(h.banner,0xcbc57608);
        const auto authored=record_model(c,"TD_Hud_ContentResources/TD_ActivityBanner_ContentWithTitle");
        c.copy(heading,c.address(authored));
        label_styles(c,c.field(heading,0x44688629),"TextStylesList/D180-Caps_White","TextStylesList/D180-Caps_White");
        h.banner_badge=make(c,texture_schema);
        c.set_if_changed(c.field(h.banner_badge,0xf524c836),256.f);
        c.set_if_changed(c.field(h.banner_badge,0x6fbd254e),256.f);
        c.set_if_changed(c.field(heading,content),Widget{asset(c,texture_widget_path),{0,h.banner_badge.handle}});
    }
    const auto heading=c.field(h.banner,0xcbc57608);
    // The title and subtitle; a title alone may carry its subtitle after a line break.
    const auto& text=hud.banner.text;const auto line=hud.banner.subtitle.empty()?text.find('\n'):std::string::npos;
    plain_label(c,c.field(heading,0x44688629),text.substr(0,line));
    plain_label(c,c.field(heading,0x00f3b15e),line==std::string::npos?hud.banner.subtitle:text.substr(line+1));
    mode_art(c,std::array{c.field(h.banner_badge,0x6e469d2a)},hud.banner.art.empty()?hud.header.art:hud.banner.art);
    mount_hud(c,h.banner,asset(c,"UI/Features/Activities/Blueprints/ActivityBanner_CenterFullWidth_Widget"),h.banner_anchor,h.banner_item,mode_hud_id+58,true);
    c.set_if_changed(c.field(h.banner_anchor,0x1cff7243),std::array<float,2>{1920.f,600.f});
}
const char* callout_effect(std::string_view style) noexcept {
    return style=="clean"?clean_outline:style=="glow"?red_glow:style=="plain"?nullptr:neon_outline;
}
// A callout top centre for a moment ("TRICK SET!").
void kit_callout(const Context& c,const kit::Hud& hud) {
    auto& h=state().kit_hud;
    const auto effect=callout_effect(hud.callout.style);
    auto& style=h.callout_styles[effect?effect:""];
    mode_card_label(c,h.callout,h.callout_text,"TextStylesList/D94-Caps-Bold_White",2,
                    custom_text_style(c,style,"TextStylesList/D94-Caps-Bold_White",effect));
    mode_card_list(c,h.callout_list,std::array{h.callout},110.f,1600.f,h.callout_anchor,h.callout_item,mode_hud_id+49);
    mode_card_place(c,h.callout_anchor,{1600.f,110.f},.5f,.5f,0.f,.24f,0.f,0.f);
}
// A boxed list top left, as the board's rows ("ALREADY BEEN DONE"): its first five items, one
// width, capped, so the box never runs off the screen; a longer item is set in a smaller native
// style rather than scrolled (a label only moves a whole character at a time, which reads as lag).
void kit_list(const Context& c,const kit::Hud& hud,bool styled) {
    auto& h=state().kit_hud;
    const auto count=std::min<std::size_t>(hud.list.items.size(),h.list_cells.size());
    constexpr unsigned widest=704;
    unsigned width=native_hud_name_width(hud.list.caption); // the caption on one line
    for(std::size_t i=0;i<count;++i)width=std::max(width,std::min(widest,native_hud_name_width(hud.list.items[i])));
    const auto small_chars=static_cast<std::size_t>((width-32)/17); // about what fits a row in SP38
    mode_card_label(c,h.list_caption,hud.list.caption,"TextStylesList/H52-XBold_White",0,
                    styled?custom_text_style(c,h.style_neon52,"TextStylesList/H52-XBold_White",neon_outline):Value{});
    std::vector<Widget> cells{{asset(c,label_widget_path),{0,h.list_caption.handle}}};
    for(std::size_t i=0;i<count;++i) {
        auto text=hud.list.items[i];
        const bool small=native_hud_name_width(text)>width;
        if(small && text.size()>small_chars && small_chars>4)text=text.substr(0,small_chars-3)+"...";
        if(!h.list_cells[i].handle)h.list_cells[i]=record_model(c,notice_record,notice_schema);
        mode_hud_cell(c,h.list_cells[i],text,static_cast<float>(width),64.f,small?"TextStylesList/SP38-SemiB_White":"TextStylesList/H52-XBold_White",
                      small || !styled?Value{}:custom_text_style(c,h.style_clean52,"TextStylesList/H52-XBold_White",clean_outline));
        cells.push_back({hud_art(c,hud.theme,"Name_"+std::to_string(width)),{0,h.list_cells[i].handle}});
    }
    if(h.list_width!=width || h.list_rows!=count || !hud_list_has_items(c,h.list_list,static_cast<unsigned>(cells.size()))) {
        hud_list(c,h.list_list,cells,false,64.f,8.f,0.f,static_cast<float>(width));
        h.list_width=width;h.list_rows=count;
    }
    mount_hud(c,h.list_list,asset(c,list_widget_path),h.list_anchor,h.list_item,mode_hud_id+50,false);
    mode_card_place(c,h.list_anchor,{static_cast<float>(width),72.f*static_cast<float>(count+1)},.03f,0.f,0.f,.12f,0.f,0.f);
}
// A card on the left: a caption, a picture (a texture path) and a title beside it.
void kit_card(const Context& c,const kit::Hud& hud,bool styled) {
    auto& h=state().kit_hud;const auto& card=hud.card;
    mode_card_label(c,h.card_caption,card.caption,"TextStylesList/H52-XBold_White",0,
                    styled?custom_text_style(c,h.style_clean52,"TextStylesList/H52-XBold_White",clean_outline):Value{});
    mode_card_list(c,h.card_caption_list,std::array{h.card_caption},60.f,1400.f,h.card_caption_anchor,h.card_caption_item,mode_hud_id+51);
    mode_card_place(c,h.card_caption_anchor,{1400.f,60.f},.03f,0.f,0.f,.42f,0.f,0.f);
    // The picture's model, set up as the Throwdown Panel's setting icons are (those draw): its size,
    // its own layout in the slot (weight 1: zero collapses it) and a colour style.
    if(!h.card_image.handle) {
        h.card_image=make(c,texture_schema);
        c.set(c.field(h.card_image,0xf524c836),140.f);c.set(c.field(h.card_image,0x6fbd254e),140.f);
        for(const auto axis:{0xde7d30c7U,0x185a5736U}) {
            const bool vertical=axis==0x185a5736U;
            for(const auto edge:{0x99bbed5aU,0x49943bedU})c.set(c.path(h.card_image,{axis,edge}),vertical?.5f:0.f);
            c.set(c.path(h.card_image,{axis,0xfec5e0b1}),vertical?.5f:0.f);
            c.set(c.path(h.card_image,{axis,0xd01f4a21}),1.f);
        }
        c.set(c.field(h.card_image,0xcd4a53c0),record_ref(c,"InteractivityColorStylesList/InteractiveColors.Core.AllWhite"));
    }
    Address image{};
    if(!card.image.empty()) try {
        const auto found=asset(c,card.image.c_str());
        const auto type=read<Address>(found+8);
        if(type==c.base+build::native_menu::texture_asset_type || type==c.base+build::native_menu::image_asset_type)image=found;
    } catch(const std::exception&) {
        static std::set<std::string> reported;
        if(reported.insert(card.image).second)logging::log(logging::Level::info,logging::Channel::ui,
            "Throwdown HUD: card picture {} is not loaded.",card.image);
    }
    h.card_image_visible=image!=0;
    if(image) {
        // skate.'s own texture widget, inside a native list as the labels are.
        if(read<Address>(c.address(c.field(h.card_image,0x6e469d2a)))!=image)c.set(c.field(h.card_image,0x6e469d2a),image);
        if(!h.card_image_list.handle || !hud_list_has_items(c,h.card_image_list,1))
            hud_list(c,h.card_image_list,{{asset(c,texture_widget_path),{0,h.card_image.handle}}},false,140.f,0.f,0.f,140.f);
        mount_hud(c,h.card_image_list,asset(c,list_widget_path),h.card_image_anchor,h.card_image_item,mode_hud_id+47,false);
        mode_card_place(c,h.card_image_anchor,{140.f,140.f},.03f,0.f,0.f,.42f,0.f,64.f);
    }
    h.card_title_visible=!card.title.empty();
    if(h.card_title_visible) {
        mode_card_label(c,h.card_title,card.title,"TextStylesList/H64-XBold_White",0,
                        styled?custom_text_style(c,h.style_clean64,"TextStylesList/H64-XBold_White",clean_outline):Value{});
        mode_card_list(c,h.card_title_list,std::array{h.card_title},72.f,1300.f,h.card_title_anchor,h.card_title_item,mode_hud_id+48);
        mode_card_place(c,h.card_title_anchor,{1300.f,72.f},.03f,0.f,image?160.f:0.f,.42f,0.f,98.f);
    }
}
void kit_intro(const Context& c,const kit::Hud& hud) {
    auto& h=state().kit_hud;
    const auto label_widget=asset(c,label_widget_path);
    const auto list_widget=asset(c,list_widget_path);
    constexpr float width=960.f,icon=112.f;
    const float title=std::min(760.f,float(native_hud_header_geometry(hud.intro.title).title_width));
    const float group=icon+24.f+title;
    if(!h.intro_badge.handle)h.intro_badge=make(c,texture_schema);
    c.set_if_changed(c.field(h.intro_badge,0xf524c836),icon);c.set_if_changed(c.field(h.intro_badge,0x6fbd254e),icon);
    c.set_if_changed(c.field(h.intro_badge,0xd1997d0c),false);
    mode_art(c,std::array{c.field(h.intro_badge,0x6e469d2a)},hud.header.art);
    mode_card_label(c,h.intro_heading,native_hud_title_text(hud.intro.title),"TextStylesList/D78-Caps-Bold_Bright",0,
        custom_text_style(c,h.intro_style,"TextStylesList/D78-Caps-Bold_Bright",nullptr));
    // Label_Widget measures itself to its text. Its list presenter can
    // therefore pull two lines up into the header despite a taller cell.
    // Use a private, fixed-height native notice label for the rules.
    if(!h.intro_rules.handle)h.intro_rules=record_model(c,notice_record,notice_schema);
    mode_hud_cell(c,h.intro_rules,hud.intro.rules,width,128.f,"TextStylesList/SP38-SemiB_White");
    c.set_if_changed(c.path(h.intro_rules,{0x94703efc,0xc13ba637}),true);
    const auto texture=asset(c,texture_widget_path);
    if(!hud_list_has_items(c,h.intro_row,2))hud_list(c,h.intro_row,
        {{texture,{0,h.intro_badge.handle}},{label_widget,{0,h.intro_heading.handle}}},true,128.f,24.f,0.f,group);
    mode_hud_list_cell_size(c,h.intro_row,0,icon,128.f,{texture,{0,h.intro_badge.handle}});
    mode_hud_list_cell_size(c,h.intro_row,1,title,128.f,{label_widget,{0,h.intro_heading.handle}});
    // All three rectangles share one explicit top-left origin. No child
    // uses a content-sized vertical pivot to determine its position.
    mount_hud(c,h.intro_row,list_widget,h.intro_anchor,h.intro_item,mode_hud_id+3,true);
    mode_card_place(c,h.intro_anchor,{group,128.f},.5f,0.f,0.f,.5f,0.f,0.f);
    mode_card_translate(c,h.intro_anchor,-group*.5f,-168.f);
    mount_hud(c,h.intro_rules,asset(c,"UI/ReSkate/Throwdowns/IntroRules_Widget"),h.intro_rules_anchor,h.intro_rules_item,mode_hud_id+56,true);
    mode_card_place(c,h.intro_rules_anchor,{width,128.f},.5f,0.f,0.f,.5f,0.f,0.f);
    mode_card_translate(c,h.intro_rules_anchor,-width*.5f,32.f);
    const auto frame="UI/ReSkate/Throwdowns/IntroPanel_"+(hud.intro.art_key.empty()?hud.theme:hud.intro.art_key)+"_Widget";
    mount_hud(c,{},asset(c,frame.c_str()),h.intro_background_anchor,h.intro_background_item,mode_hud_id+55,true);
    c.set_if_changed(c.field(h.intro_background_anchor,widget),Widget{asset(c,frame.c_str()),{}});
    mode_card_place(c,h.intro_background_anchor,{1088.f,448.f},.5f,0.f,0.f,.5f,0.f,0.f);
    mode_card_translate(c,h.intro_background_anchor,-544.f,-224.f);
}
void kit_countdown(const Context& c,std::uint64_t remaining,bool go) {
    auto& h=state().kit_hud;
    if(!h.countdown.handle)h.countdown=record_model(c,"Activities_HudElementData_ContentResource/Activities_CountdownItemHUD");
    // ActivityCountdownViewModel drives the stock grunge/scribble transitions of
    // the game's turn-start 3-2-1-GO.
    plain_label(c,c.field(h.countdown,0x58c9d354),go?"GO!":std::to_string(native_hud_seconds(remaining)));
    c.set_if_changed(c.field(h.countdown,1698918111U),go?0.f:std::clamp(static_cast<float>(remaining)/3000.f,0.f,1.f));
    mount_hud(c,h.countdown,asset(c,"UI/Features/Activities/Widgets/System/ActivityCountdown_Widget"),h.countdown_anchor,h.countdown_item,mode_hud_id+2,true);
    c.set_if_changed(c.field(h.countdown_anchor,0x1cff7243),std::array<float,2>{512.f,512.f});
}
void kit_blackout(const Context& c) {
    auto& h=state().kit_hud;
    mount_hud(c,{},asset(c,"UI/ReSkate/Throwdowns/SeekerBlackout_Widget"),h.blind_anchor,h.blind_item,mode_hud_id+4,true);
    c.set_if_changed(c.field(h.blind_anchor,0x1cff7243),std::array<float,2>{1920.f,1080.f});
    for(const auto axis:{0xde7d30c7U,0x185a5736U}) {
        const auto layout=c.field(h.blind_anchor,axis);
        c.set_if_changed(c.field(layout,0x99bbed5a),0.f);c.set_if_changed(c.field(layout,0x49943bed),1.f);
        c.set_if_changed(c.field(layout,0x40bfbff4),0.f);c.set_if_changed(c.field(layout,0x1d233266),0.f);
        c.set_if_changed(c.field(layout,0xfec5e0b1),.5f);
    }
}
// Markers over the world: each point projected with the camera the client last used, clamped to
// the screen, with its distance from the local skater when asked.
void kit_markers(const Context& c,const kit::Hud& hud) {
    auto& h=state().kit_hud;h.world_items.clear();
    const auto view=latest_game_view();
    if(hud.markers.empty() || !view || !(view->vertical_fov>1 && view->vertical_fov<175))return;
    const auto& camera=view->world;
    static float aspect=16.f/9.f;
    DWORD process{};const auto window=GetForegroundWindow();GetWindowThreadProcessId(window,&process);
    RECT client{};
    if(process==GetCurrentProcessId() && GetClientRect(window,&client) && client.bottom>client.top)
        aspect=static_cast<float>(client.right-client.left)/static_cast<float>(client.bottom-client.top);
    const auto position=[&](const kit::Vec3& p)->std::optional<std::array<float,2>> {
        const std::array<float,3> delta{p[0]-camera[12],p[1]+1.5f-camera[13],p[2]-camera[14]};
        const auto dot=[&](unsigned i){return delta[0]*camera[i]+delta[1]*camera[i+1]+delta[2]*camera[i+2];};
        const float depth=-dot(8);if(depth<=.1f)return {};
        const float focal=1.f/(2.f*std::tan(view->vertical_fov*3.14159265f/360.f));
        const float x=.5f+dot(0)*focal/depth/aspect,y=.5f-dot(4)*focal/depth;
        if(!std::isfinite(x) || !std::isfinite(y))return {};
        return std::array<float,2>{std::clamp(x,.08f,.92f),std::clamp(y,.08f,.9f)};
    };
    // Distances are the local skater's, as the trainer's telemetry has it; the camera's without one.
    const auto skater=trainer::telemetry();
    const std::array<float,3> from=skater.skater?skater.position:std::array<float,3>{camera[12],camera[13],camera[14]};
    for(const auto& marker_doc:hud.markers) {
        if(h.world_items.size()>=h.markers.size())break;
        const auto xy=position(marker_doc.at);if(!xy)continue;
        const auto index=static_cast<unsigned>(h.world_items.size());auto& marker=h.markers[index];
        if(!marker.notice.handle)marker.notice=record_model(c,notice_record,notice_schema);
        auto caption=marker_doc.text;
        if(marker_doc.distance) {
            float distance{};for(unsigned axis=0;axis<3;++axis)distance+=(marker_doc.at[axis]-from[axis])*(marker_doc.at[axis]-from[axis]);
            caption+="  "+std::to_string(static_cast<int>(std::sqrt(distance)))+" m";
        }
        plain_label(c,c.field(marker.notice,0x94703efc),caption);
        label_styles(c,c.field(marker.notice,0x94703efc),"TextStylesList/H52-XBold_White","TextStylesList/H52-XBold_White");
        c.set(c.field(marker.notice,0x13be0d51),std::array<float,2>{320.f,82.f});
        mount_hud(c,marker.notice,asset(c,"UI/Foundations/Components/Notices/BasicNotice_Widget"),marker.anchor,marker.item,mode_hud_id+8+static_cast<int>(index),true);
        c.set(c.field(marker.anchor,0x1cff7243),std::array<float,2>{320.f,82.f});
        for(unsigned axis=0;axis<2;++axis)for(const auto field:{0x99bbed5aU,0x49943bedU})
            c.set(c.path(marker.anchor,{axis?0x185a5736U:0xde7d30c7U,field}),(*xy)[axis]);
        h.world_items.push_back(marker.item);
    }
}
void compass_position(const Context& c,KitHud& h,float offset) {
    constexpr float size=native_hud_compass_icon;
    c.set_if_changed(c.field(h.compass_anchor,0x1cff7243),std::array<float,2>{size,size});
    c.set_if_changed(c.field(h.compass_anchor,0x6efc1a61),false);
    c.set_if_changed(c.field(h.compass_anchor,0xbe5683d9),false);
    for(const auto axis:{0xde7d30c7U,0x185a5736U}) {
        const bool vertical=axis==0x185a5736U;
        // Center the icon on the native compass strip.
        for(const auto key:{0x99bbed5aU,0x49943bedU})c.set_if_changed(c.path(h.compass_anchor,{axis,key}),vertical?0.f:.5f);
        const auto offsets=native_hud_translation(vertical?native_hud_compass_center_y:offset);
        c.set_if_changed(c.path(h.compass_anchor,{axis,0x40bfbff4U}),offsets[0]);
        c.set_if_changed(c.path(h.compass_anchor,{axis,0x1d233266U}),offsets[1]);
        c.set_if_changed(c.path(h.compass_anchor,{axis,0xfec5e0b1}),.5f);
        c.set_if_changed(c.path(h.compass_anchor,{axis,0xd01f4a21}),1.f);
    }
}
void kit_compass_motion(const Context& c,const kit::Hud& hud) {
    auto& h=state().kit_hud;
    if(!h.compass_visible || !hud.compass.present || !h.compass_anchor.handle ||
       c.type_of(h.compass_anchor.handle)!=h.compass_anchor.type)return;
    const auto view=latest_game_view();if(!view)return;
    const auto offset=native_hud_compass_offset(hud.compass.at,view->world);if(!offset)return;
    compass_position(c,h,*offset);
}
void kit_compass(const Context& c,const kit::Hud& hud) {
    auto& h=state().kit_hud;h.compass_visible=false;
    const auto view=latest_game_view();if(!view)return;
    const auto offset=native_hud_compass_offset(hud.compass.at,view->world);if(!offset)return;
    if(!h.compass.handle)h.compass=make(c,texture_schema);
    constexpr float size=native_hud_compass_icon;
    c.set_if_changed(c.field(h.compass,0xf524c836),size);c.set_if_changed(c.field(h.compass,0x6fbd254e),size);
    c.set_if_changed(c.field(h.compass,0xd1997d0c),false);
    c.set_if_changed(c.field(h.compass,0xcd4a53c0),record_ref(c,"InteractivityColorStylesList/InteractiveColors.Core.AllWhite"));
    mode_art(c,std::array{c.field(h.compass,0x6e469d2a)},hud.compass.art);
    const auto texture_widget=asset(c,texture_widget_path);
    if(!hud_list_has_items(c,h.compass_list,1))hud_list(c,h.compass_list,{{texture_widget,{0,h.compass.handle}}},true,size,0.f);
    mode_hud_list_cell_size(c,h.compass_list,0,size,size,{texture_widget,{0,h.compass.handle}});
    mount_hud(c,h.compass_list,asset(c,list_widget_path),h.compass_anchor,h.compass_item,mode_hud_id+57,false);
    compass_position(c,h,*offset);
    h.compass_visible=true;
}
// Change only an owned text style. Validate the four-channel color layout
// before finding alpha; unknown reflection layouts retain fully visible text.
void placement_text_alpha(const Context& c,Value style,float alpha) {
    if(!style.handle)return;
    const auto color=c.path(style,{0xc7a2ba80,0x2358a817,0xd32f6895});
    if(kind(color.type)!=2 || size(color.type)!=16)return;
    const auto metadata=read<Address>(color.type),fields=read<Address>(metadata+0x60);
    if(read<std::uint16_t>(metadata+0x2a)!=4 || !fields)return;
    const std::array<std::uint32_t,3> rgb{0x6fb7b2a4,0x3b385eb0,0xd1cdb49a};
    unsigned found{};std::uint32_t alpha_hash{};
    for(unsigned i=0;i<4;++i) {
        const auto field=fields+i*24;const auto hash=read<std::uint32_t>(field);
        if(size(read<Address>(field+16))!=4)return;
        if(std::find(rgb.begin(),rgb.end(),hash)!=rgb.end())++found;else alpha_hash=hash;
    }
    if(found==3 && alpha_hash)c.set_if_changed(c.field(color,alpha_hash),std::clamp(alpha,0.f,1.f));
}
// An input's glyph: a private LabelButton bound to that input, so skate. draws the device's own
// glyph. Built from immutable donor styling only, never its backend caption or action graph.
Value editor_glyph(const Context& c,const std::string& input_name) {
    auto& h=state().kit_hud;auto& glyph=h.glyphs[input_name];
    if(!glyph.handle) {
        const auto checkpoint=state().owned.checkpoint();
        try {
            const auto donor=record_model(c,"Throwdown_DetailsMenu_ContentResource/ThrowdownDetails_HUDButton",button);
            const auto candidate=make(c,button);
            for(const auto key:{0x8cc042efU,0xf9515817U,0xf343d160U})
                c.copy(c.field(candidate,key),c.address(c.field(donor,key)));
            c.copy(c.path(candidate,{0x0fb0d794,0x8cc042ef}),c.address(c.path(donor,{0x0fb0d794,0x8cc042ef})));
            PrivateUiGraph graph{c};graph.walk(candidate,0);
            clear_value(c,c.field(candidate,0x67212c85));
            const auto action=c.path(candidate,{0x0fb0d794,0xa704272a,0xc52416ef,0xa704272a,0xa2b71c93});
            clear_value(c,c.field(action,0x9c76b86c));
            c.text(c.field(action,0xb1e14cd6),"");
            const auto interact=c.path(candidate,{0x0fb0d794,0xa704272a,0xc52416ef,0xa704272a});
            // DingoInteractable reflection fields, checked against the
            // game's native type map; graph property hashes differ.
            c.set_if_changed(c.field(interact,0x38f7355f),true);
            c.set_if_changed(c.field(interact,0x4b3146e9),false);
            label_styles(c,c.field(candidate,0x58c9d354),"TextStylesList/SP48-SemiB_White_DropShadow75","TextStylesList/SP48-SemiB_White_DropShadow75");
            glyph=candidate; // Publish only a complete prompt.
        } catch(...) {
            h.glyphs.erase(input_name);
            state().owned.rollback(checkpoint,[&](Value value){c.destroy(value);});
            throw;
        }
    }
    // Native input/device changes may rebind the widget. Reconcile our
    // private caption and PIA without recreating its presenter/list.
    plain_label(c,c.field(glyph,0x58c9d354),"");
    const auto action=c.path(glyph,{0x0fb0d794,0xa704272a,0xc52416ef,0xa704272a,0xa2b71c93});
    const auto prompt_input=asset(c,("Configurations/Input/UI/"+input_name).c_str());
    for(const auto key:{0x3d00de27U,0x0169c2dbU})
        if(read<Address>(c.address(c.field(action,key)))!=prompt_input)c.set_if_changed(c.field(action,key),prompt_input);
    return glyph;
}
// The editor: a controls panel with one row per prompt (its input glyphs, a pair side by side, and
// its caption), an information panel, a fading toast and a confirmation. Variant "builder" is the
// large panel (each row mounted at its own offset), "compact" the small one; without a variant,
// a document with information rows is the builder.
void kit_editor(const Context& c,const kit::Hud& hud,std::uint64_t) {
    auto& h=state().kit_hud;const auto& e=hud.editor;
    const auto label_widget=asset(c,label_widget_path);
    const auto list_widget=asset(c,list_widget_path);
    const bool builder=e.variant=="builder" || (e.variant!="compact" && (!e.info.empty() || !e.info_title.empty()));
    const bool created=h.editor_builder!=builder; // the rows differ: rebuild them on a switch
    h.editor_builder=builder;
    const auto panel=asset(c,builder?"UI/ReSkate/Throwdowns/EditorControlsPanel_Widget":"UI/ReSkate/Throwdowns/EditorPanel_Widget");
    mount_hud(c,{},panel,h.editor_panel_anchor,h.editor_panel_item,mode_hud_id+43,false);
    mode_card_place(c,h.editor_panel_anchor,builder?std::array<float,2>{704.f,1000.f}:std::array<float,2>{500.f,370.f},
        native_hud_screen_anchor(builder?.022f:.006f),0.f,0.f,native_hud_screen_anchor(builder?.04f:.008f),0.f,0.f);
    c.set_if_changed(c.field(h.editor_panel_anchor,widget),Widget{panel,{}});
    const auto glyph_widget=asset(c,"UI/ReSkate/Throwdowns/EditorGlyph_Widget");
    const auto caption_widget=asset(c,builder?"UI/ReSkate/Throwdowns/EditorControlCaption_Widget":"UI/ReSkate/Throwdowns/EditorCaption_Widget");
    const float line_height=builder?64.f:56.f,line_width=builder?656.f:820.f;
    std::vector<Widget> lines;
    const auto count=std::min<std::size_t>(e.prompts.size(),builder?h.editor_row_anchors.size():h.lines.size());
    for(std::size_t i=0;i<count;++i) {
        const auto& prompt=e.prompts[i];
        if(prompt.inputs.empty())continue;
        const bool pair=prompt.inputs.size()>=2;
        const Widget first{glyph_widget,{0,editor_glyph(c,prompt.inputs[0]).handle}};
        Widget icons=first;
        if(pair) {
            const Widget second{glyph_widget,{0,editor_glyph(c,prompt.inputs[1]).handle}};
            const auto key=prompt.inputs[0]+"|"+prompt.inputs[1];
            if(!h.pair_lists[i].handle || created || h.pair_keys[i]!=key) {
                hud_list(c,h.pair_lists[i],{first,second},true,56.f,8.f,0.f,120.f,56.f);h.pair_keys[i]=key;
            }
            icons={list_widget,{0,h.pair_lists[i].handle}};
        }
        auto& label=h.line_captions[i];
        if(!label.handle) {
            label=record_model(c,header_label_record);
            label_styles(c,label,"TextStylesList/SP38-SemiB_White","TextStylesList/SP38-SemiB_White");
            c.set_if_changed(c.field(label,0xc13ba637),false);c.set_if_changed(c.field(label,0xfc53d427),false);
            c.set_if_changed(c.field(label,0x87ad624c),2);c.set_if_changed(c.field(label,0x3d8639d0),0);
        }
        plain_label(c,label,prompt.caption);
        auto& line=h.lines[i];
        const auto key=std::string(pair?"pair:":"one:")+prompt.inputs[0];
        if(!line.handle || created || h.line_keys[i]!=key) {
            const Widget text{caption_widget,{0,label.handle}};
            std::vector<Widget> cells{icons,text};
            hud_list(c,line,cells,true,line_height,12.f,0.f,line_width,pair?120.f:56.f);
            const auto refs=c.field(line,0xf2c90867);
            for(unsigned cell=0;cell<cells.size();++cell) {
                const auto handle=read<Handle>(c.address(c.element(refs,cell)));
                const float width=cell+1==cells.size()?(builder?524.f:500.f):builder?120.f:(pair?120.f:56.f);
                c.set_if_changed(c.field({handle,c.type_of(handle)},0x1cff7243),std::array<float,2>{width,line_height});
            }
            h.line_keys[i]=key;
        }
        // A prompt's glyph names the widget its first input is drawn with ("EditorClearGlyph12": a
        // hold ring). Without one, a hold prompt shows its progress ring (not under a confirmation).
        Widget shown=icons;
        if(!prompt.glyph.empty())shown={asset(c,("UI/ReSkate/Throwdowns/"+prompt.glyph+"_Widget").c_str()),first.data};
        else if(prompt.hold>0.f && e.modal.empty()) {
            const auto frame=std::clamp(static_cast<unsigned>(std::ceil(std::clamp(prompt.hold,0.f,1.f)*32.f)),1u,32u);
            shown={asset(c,("UI/ReSkate/Throwdowns/EditorClearGlyph"+std::to_string(frame)+"_Widget").c_str()),first.data};
        }
        if(builder) {
            mode_hud_list_cell_size(c,line,0,120.f,64.f,shown);
            mode_hud_list_cell_size(c,line,1,524.f,64.f,{caption_widget,{0,label.handle}});
        } else {
            const auto handle=read<Handle>(c.address(c.element(c.field(line,0xf2c90867),0)));
            c.set_if_changed(c.field({handle,c.type_of(handle)},widget),shown);
        }
        lines.push_back({list_widget,{0,line.handle}});
    }
    h.editor_rows=static_cast<unsigned>(lines.size());
    if(builder) {
        // Mount each bounded row at an explicit offset. The native linear
        // list can expand its measured stride after glyph/device changes.
        // Its content must not push the final action below the panel.
        for(unsigned row=0;row<lines.size();++row) {
            mount_hud(c,h.lines[row],list_widget,h.editor_row_anchors[row],h.editor_row_items[row],mode_hud_id+64+static_cast<int>(row),false);
            mode_card_place(c,h.editor_row_anchors[row],{656.f,64.f},native_hud_screen_anchor(.022f),0.f,0.f,native_hud_screen_anchor(.04f),0.f,0.f);
            mode_card_translate(c,h.editor_row_anchors[row],24.f,104.f+80.f*static_cast<float>(row));
        }
        if(!h.editor_title.handle)h.editor_title=record_model(c,header_label_record);
        label_styles(c,h.editor_title,"TextStylesList/H52-XBold_White","TextStylesList/H52-XBold_White");
        plain_label(c,h.editor_title,e.title);
        lines={{asset(c,"UI/ReSkate/Throwdowns/EditorBuilderTitle_Widget"),{0,h.editor_title.handle}}};
    } else {
        if(!h.editor_title.handle)h.editor_title=record_model(c,header_label_record);
        label_styles(c,h.editor_title,"TextStylesList/H52-XBold-Title_Bright","TextStylesList/H52-XBold-Title_Bright");
        plain_label(c,h.editor_title,e.title);
        lines.insert(lines.begin(),Widget{label_widget,{0,h.editor_title.handle}});
    }
    c.set_if_changed(c.field(h.editor_title,0xc13ba637),false);c.set_if_changed(c.field(h.editor_title,0xfc53d427),false);
    c.set_if_changed(c.field(h.editor_title,0x87ad624c),2);c.set_if_changed(c.field(h.editor_title,0x3d8639d0),0);
    std::vector<Handle> composition;
    for(const auto& line:lines)composition.push_back(line.data.handle);
    if(created || h.editor_composition!=composition || !hud_list_has_items(c,h.editor_list,static_cast<unsigned>(lines.size()))) {
        hud_list(c,h.editor_list,lines,false,line_height,8.f,0.f,line_width);h.editor_composition=composition;
    }
    mount_hud(c,h.editor_list,list_widget,h.editor_anchor,h.editor_item,mode_hud_id+7,false);
    mode_card_place(c,h.editor_anchor,builder?std::array<float,2>{656.f,64.f}:std::array<float,2>{820.f,930.f},
        native_hud_screen_anchor(builder?.022f:.006f),0.f,0.f,native_hud_screen_anchor(builder?.04f:.008f),0.f,0.f);
    mode_card_translate(c,h.editor_anchor,24.f,24.f);
    // Information: its title and "caption: value" lines in the panel on the right.
    h.info_visible=builder;
    if(builder) {
        for(unsigned i=0;i<h.info_labels.size();++i) {
            std::string text;
            if(i==0)text=e.info_title;
            else if(i-1<e.info.size())text=e.info[i-1].second.empty()?e.info[i-1].first:e.info[i-1].first+": "+e.info[i-1].second;
            mode_card_label(c,h.info_labels[i],text,"TextStylesList/SP38-SemiB_White",0);
        }
        mode_card_list(c,h.info_list,h.info_labels,52.f,544.f,h.info_anchor,h.info_item,mode_hud_id+53);
        mode_card_place(c,h.info_anchor,{544.f,444.f},native_hud_screen_anchor(.98f),1.f,0.f,.34f,0.f,0.f);
        mode_card_translate(c,h.info_anchor,-28.f,24.f);
        mount_hud(c,{},asset(c,"UI/ReSkate/Throwdowns/EditorInfoPanel_Widget"),h.info_panel_anchor,h.info_panel_item,mode_hud_id+54,false);
        mode_card_place(c,h.info_panel_anchor,{600.f,492.f},native_hud_screen_anchor(.98f),1.f,0.f,.34f,0.f,0.f);
    }
    // The toast: one line centre top, as opaque as the document says.
    h.toast_visible=!e.toast.empty() && e.toast_alpha>0.f;
    if(h.toast_visible) {
        const auto style=custom_text_style(c,h.toast_style,"TextStylesList/H52-XBold_White",clean_outline);
        placement_text_alpha(c,style,e.toast_alpha);
        mode_card_label(c,h.toast,e.toast,"TextStylesList/H52-XBold_White",2,style);
        mode_card_list(c,h.toast_list,std::array{h.toast},68.f,850.f,h.toast_anchor,h.toast_item,mode_hud_id+52);
        mode_card_place(c,h.toast_anchor,{850.f,72.f},.5f,.5f,0.f,0.f,0.f,300.f);
    }
    // The confirmation: its text and up to two actions (glyph and caption).
    h.modal_visible=!e.modal.empty();
    if(h.modal_visible) {
        mount_hud(c,{},asset(c,"UI/ReSkate/Throwdowns/ConfirmationPanel_Widget"),h.modal_panel_anchor,h.modal_panel_item,mode_hud_id+44,true);
        if(!h.modal_text.handle){h.modal_text=record_model(c,header_label_record);label_styles(c,h.modal_text,"TextStylesList/H52-XBold_White","TextStylesList/H52-XBold_White");}
        plain_label(c,h.modal_text,e.modal);
        c.set_if_changed(c.field(h.modal_text,0xc13ba637),true);
        std::vector<Widget> modal{{label_widget,{0,h.modal_text.handle}}};
        std::string key;
        for(std::size_t i=0;i<e.modal_actions.size() && i<h.modal_actions.size();++i) {
            const auto& action=e.modal_actions[i];
            if(action.inputs.empty())continue;
            auto& caption=h.modal_captions[i];
            if(!caption.handle){caption=record_model(c,header_label_record);label_styles(c,caption,"TextStylesList/SP48-SemiB_White_DropShadow75","TextStylesList/SP48-SemiB_White_DropShadow75");}
            plain_label(c,caption,action.caption);
            const auto glyph=editor_glyph(c,action.inputs[0]);
            const auto action_glyph=action.glyph.empty()?glyph_widget:asset(c,("UI/ReSkate/Throwdowns/"+action.glyph+"_Widget").c_str());
            const auto action_key=action.inputs[0]+"|"+action.glyph;
            if(h.modal_keys[i]!=action_key || !hud_list_has_items(c,h.modal_actions[i],2)) {
                hud_list(c,h.modal_actions[i],{{action_glyph,{0,glyph.handle}},
                    {asset(c,"UI/ReSkate/Throwdowns/EditorCaption_Widget"),{0,caption.handle}}},true,56.f,20.f,0.f,800.f,56.f);
                h.modal_keys[i]=action_key;
            }
            modal.push_back({list_widget,{0,h.modal_actions[i].handle}});key+=action.inputs[0]+"|";
        }
        if(h.modal_key!=key || !hud_list_has_items(c,h.modal_list,static_cast<unsigned>(modal.size()))) {
            hud_list(c,h.modal_list,modal,false,76.f,10.f,0.f,820.f);h.modal_key=key;
        }
        const auto rows=c.field(h.modal_list,0xf2c90867);
        const auto header=read<Handle>(c.address(c.element(rows,0)));
        c.set_if_changed(c.field({header,c.type_of(header)},0x1cff7243),std::array<float,2>{820.f,120.f});
        mount_hud(c,h.modal_list,list_widget,h.modal_anchor,h.modal_item,mode_hud_id+45,true);
        for(const auto anchor:{h.modal_panel_anchor,h.modal_anchor}) {
            c.set_if_changed(c.field(anchor,0x1cff7243),std::array<float,2>{880.f,300.f});
            for(const auto axis:{0xde7d30c7U,0x185a5736U}) {
                for(const auto field:{0x99bbed5aU,0x49943bedU})c.set_if_changed(c.path(anchor,{axis,field}),.5f);
                c.set_if_changed(c.path(anchor,{axis,0xfec5e0b1}),.5f);
                for(const auto field:{0x40bfbff4U,0x1d233266U})c.set_if_changed(c.path(anchor,{axis,field}),0.f);
            }
        }
    }
}
// What the document shows this tick, decided before any widget is built: a late asset must not keep
// an expired countdown or intro on screen.
struct KitHudVisibility { bool intro{},countdown{},go{};std::uint64_t remaining{}; };
KitHudVisibility kit_hud_visibility(KitHud& h,const kit::Hud& hud,std::uint64_t now) {
    KitHudVisibility v;
    v.intro=hud.intro.present && (!hud.intro.until_ms || now<hud.intro.until_ms);
    v.remaining=hud.countdown.present?clock_now(hud.countdown,now):0;
    const bool counting=hud.countdown.present && v.remaining>0;
    // GO for a moment once the count reaches zero, whether or not the document still has it.
    if(h.was_counting && !counting)h.go_until=now+900;
    h.was_counting=counting;
    v.go=now<h.go_until;
    v.countdown=(counting && !v.intro) || v.go;
    return v;
}
void kit_hud_transients(const Context& c,const KitHudVisibility& v) {
    auto& h=state().kit_hud;
    if(!h.stack.handle || c.type_of(h.stack.handle)!=h.stack.type ||
       !read<bool>(c.address(c.field(h.stack,0x369babfe))))return;
    for(const auto [item,keep]:std::array<std::pair<Value,bool>,4>{{
        {h.countdown_item,v.countdown},{h.intro_item,v.intro},
        {h.intro_background_item,v.intro},{h.intro_rules_item,v.intro}}}) {
        if(!keep && item.handle && c.type_of(item.handle)==item.type)
            c.set_if_changed(c.field(item,0x369babfe),false);
    }
}
void kit_hud_publish(const Context& c,const std::vector<Value>& visible) {
    auto& h=state().kit_hud;if(!h.stack.handle || c.type_of(h.stack.handle)!=h.stack.type)return;
    const bool any=!visible.empty();
    const bool ready=hud_publication_allowed(read<bool>(c.address(c.field(h.stack,0x369babfe))),state().in_menu,
        any && state().match_panel_open.load());
    const bool settled=h.publication.allow(h.stack.handle,h.generation,ready,GetTickCount64());
    if(!ready || (any && !settled)) {
        if(const auto found=state().hud_surfaces.find(h.stack.handle);found!=state().hud_surfaces.end()) {
            if(!ready)found->second.suspended=true;
            hide_hud_surface_content(c,found->second);
        }
        return;
    }
    publish_hud_surface(c,h.stack,visible,true);
}
void kit_hud(const Context& c) {
    auto& s=state();auto& h=s.kit_hud;const auto now=GetTickCount64();
    const auto docs=kit::active();
    const auto generation=docs && docs->hud.active?std::hash<std::string>{}(docs->plugin)|1:0;
    // A match that ended or another plugin's: everything goes, including a pending GO, and a banner
    // or callout of the new one shows whatever serial it starts from.
    if(h.generation!=generation) {
        kit_hud_publish(c,{});h.generation=generation;h.go_until=0;h.was_counting=false;
        h.callout_serial=h.banner_serial=~0ULL;h.callout_until=h.banner_until=0;
    }
    if(!generation) {s.hud_doc.reset();kit_hud_publish(c,{});s.hud_ready.store(false);return;}
    const auto& hud=docs->hud;
    s.hud_doc=hud;
    const auto shown=kit_hud_visibility(h,hud,now);
    kit_hud_transients(c,shown);
    if(!h.title.handle)h.title=record_model(c,header_label_record);
    label_styles(c,h.title,"TextStylesList/H64-XBold_White","TextStylesList/H64-XBold_White");
    if(!h.status.handle) {h.status=make(c,label_schema);c.copy(h.status,c.address(h.title));}
    label_styles(c,h.status,"TextStylesList/SP38-SemiB_White","TextStylesList/SP38-SemiB_White");
    for(const auto value:{h.title,h.status}) {
        c.set_if_changed(c.field(value,0xc13ba637),false);c.set_if_changed(c.field(value,0xfc53d427),false);
        c.set_if_changed(c.field(value,0x87ad624c),2);
    }
    if(h.stack.handle!=s.hud_stack.handle) {kit_hud_publish(c,{});h.stack=s.hud_stack;}
    std::vector<Value> visible;
    const auto append=[&](Value item) {if(item.handle)visible.push_back(item);};
    // Each piece retries on its own: a late asset costs that piece, never the rest of the frame.
    const auto piece=[&](const char* name,auto&& build) {
        try {build();}
        catch(const std::exception& e) {
            static std::map<std::string,std::uint64_t> reported;
            auto& at=reported[name];
            if(now>=at) {at=now+10000;logging::log(logging::Level::debug,logging::Channel::ui,"Throwdown HUD {}: {}",name,e.what());}
        }
    };
    const bool styled=hud.board.style=="outlined";
    const bool results=hud.results.present,editor=hud.editor.present,blackout=hud.blackout.present;
    if(!hud.header.title.empty())piece("header",[&] {
        const auto status=blackout && !hud.blackout.text.empty()?hud.blackout.text:hud.header.status;
        plain_label(c,h.status,status);
        if(!h.badge.handle)h.badge=make(c,texture_schema);
        h.header.title=h.title;h.header.badge=h.badge;
        challenge_header(c,h.header,hud.theme,hud.header.title,hud.header.art,status.empty()?Value{}:h.status,
            mode_hud_id,mode_hud_id+40,hud.header.offset,hud.header.icon_size,hud.header.frame_width);
        if(!results){append(h.header.frame_item);append(h.header.item);}
    });
    if(!results) {
        if(hud.compass.present)piece("compass",[&]{kit_compass(c,hud);if(h.compass_visible)append(h.compass_item);});
        else h.compass_visible=false;
        if(!editor) {
            // A "list" board holds the counters: they are built here and mounted inside it.
            const bool listed=hud.board.present && hud.board.heading=="list";
            if(!hud.counters.empty())piece("counters",[&]{kit_counters(c,hud,!listed);if(!listed)append(h.score_item);});
            if(hud.clock.present)piece("clock",[&]{kit_clock(c,hud,now);append(h.clock_item);});
            if(hud.board.present)piece("board",[&]{kit_board(c,hud,styled);append(h.board_item);});
            if(hud.position.present)piece("position",[&]{kit_position(c,hud);append(h.position_item);});
            if(!hud.timers.empty())piece("timers",[&] {
                kit_timers(c,hud,now);
                for(std::size_t i=0;i<hud.timers.size() && i<2;++i)append(h.timer_items[i]);
                if(captioned_timer(hud.timers)!=SIZE_MAX)append(h.timer_caption_item);
            });
            if(hud.card.present)piece("card",[&] {
                kit_card(c,hud,styled);append(h.card_caption_item);
                if(h.card_image_visible)append(h.card_image_item);
                if(h.card_title_visible)append(h.card_title_item);
            });
            if(hud.list.present && !hud.list.items.empty())piece("list",[&]{kit_list(c,hud,styled);append(h.list_item);});
        }
        if(!hud.warning.text.empty())piece("warning",[&]{kit_warning(c,hud);append(h.warning_item);});
        // A callout and a banner each show for their time after a new serial. A document without the
        // slot leaves the serial as it was: the same one coming back is not news.
        if(!hud.callout.text.empty() && hud.callout.serial!=h.callout_serial) {
            h.callout_serial=hud.callout.serial;
            h.callout_until=now+(hud.callout.ms?hud.callout.ms:2500);h.callout_text=hud.callout.text;
        }
        if(now<h.callout_until)piece("callout",[&]{kit_callout(c,hud);append(h.callout_item);});
        if(!hud.banner.text.empty() && hud.banner.serial!=h.banner_serial) {
            h.banner_serial=hud.banner.serial;
            h.banner_until=now+(hud.banner.ms?hud.banner.ms:3000);
        }
        if(now<h.banner_until && !hud.banner.text.empty())piece("banner",[&]{kit_banner(c,hud);append(h.banner_item);});
        if(editor)piece("editor",[&] {
            kit_editor(c,hud,now);
            append(h.editor_panel_item);append(h.editor_item);
            if(h.editor_builder)for(unsigned i=0;i<h.editor_rows;++i)append(h.editor_row_items[i]);
            if(h.info_visible){append(h.info_panel_item);append(h.info_item);}
            if(h.toast_visible)append(h.toast_item);
            if(h.modal_visible){append(h.modal_panel_item);append(h.modal_item);}
        });
        if(!blackout)piece("markers",[&]{kit_markers(c,hud);for(const auto item:h.world_items)append(item);});
    } else piece("results",[&]{kit_results(c,hud);append(h.results_panel_item);append(h.results_item);});
    if(shown.intro)piece("intro",[&] {
        kit_intro(c,hud);append(h.intro_background_item);append(h.intro_rules_item);append(h.intro_item);
    });
    if(shown.countdown)piece("countdown",[&]{kit_countdown(c,shown.remaining,shown.go);append(h.countdown_item);});
    if(blackout)piece("blackout",[&]{kit_blackout(c);append(h.blind_item);});
    kit_hud_publish(c,visible);
    if(!s.hud_ready.exchange(true))logging::write(logging::Level::info,logging::Channel::ui,"Throwdown HUD: the active plugin's document is on the native HUD.");
}
// Frame-rate refresh between full HUD ticks: only ticking clocks, timers and the compass move.
// Re-publishing a stack here would race its visibility/layout transitions.
void kit_hud_motion(const Context& c,std::uint64_t now) {
    auto& s=state();auto& h=s.kit_hud;
    if(!s.hud_doc || !kit_hud_ticks(*s.hud_doc) || s.in_menu || !h.stack.handle || c.type_of(h.stack.handle)!=h.stack.type ||
       !hud_publication_allowed(read<bool>(c.address(c.field(h.stack,0x369babfe))),s.in_menu,s.match_panel_open.load()))return;
    kit_hud_clocks(c,*s.hud_doc,now);
    kit_compass_motion(c,*s.hud_doc);
}
