// Private Throwdown details inside the native activity layer, from the running match's kit Panel
// document. The native challenge navigation owns the camera, input context and Back transition.
struct MatchPanelView {
    bool available{};
    std::string plugin,mode,identity;
    kit::Panel panel;
};
// A player's Steam profile picture, for the native web image in their panel row. Their public
// Steam profile names it (no key needed); skate. then downloads it from Steam's image servers,
// whose Let's Encrypt certificates ReSkate already trusts for news images. Looked up once per
// player in the background: empty until then, and for a lookup that failed.
struct SteamPictures { std::mutex mutex; std::map<std::uint64_t,std::string> urls; std::set<std::uint64_t> asked; };
SteamPictures& steam_pictures() { static auto* value=new SteamPictures; return *value; }
bool steam_image_url(std::string_view url) {
    return url.starts_with("https://avatars.") && url.find(".steamstatic.com/")!=std::string_view::npos;
}
std::string steam_picture(std::uint64_t id) {
    if(!individual_steam_id(id))return {};
    auto& pictures=steam_pictures();
    {
        std::lock_guard lock(pictures.mutex);
        if(const auto found=pictures.urls.find(id);found!=pictures.urls.end())return found->second;
        if(!pictures.asked.insert(id).second)return {};
    }
    std::thread([id] {
        std::string url;
        try {
            const auto profile=https::get_text(L"https://steamcommunity.com/profiles/"+std::to_wstring(id)+L"?xml=1",512*1024,10,L"ReSkate/1");
            constexpr std::string_view open="<avatarFull><![CDATA[";
            if(profile)if(const auto at=profile->find(open);at!=std::string::npos) {
                const auto start=at+open.size(),end=profile->find("]]>",start);
                if(end!=std::string::npos)url=profile->substr(start,end-start);
            }
            // Only Steam's own image servers.
            if(!steam_image_url(url))url.clear();
        } catch(...) {url.clear();}
        logging::log(logging::Level::info,logging::Channel::ui,"Steam picture for {}: {}",id,url.empty()?std::string("none"):url);
        auto& pictures=steam_pictures();std::lock_guard lock(pictures.mutex);pictures.urls[id]=url;
    }).detach();
    return {};
}
// The mode a plugin's documents are about, for its events: its waiting card's, its setup page's,
// or the native flag's while that flag is the plugin's.
std::string documents_mode(const kit::Documents& docs) {
    if(docs.waiting && !docs.waiting->mode.empty())return full_mode(docs.plugin,docs.waiting->mode);
    if(docs.setup && !docs.setup->mode.empty())return full_mode(docs.plugin,docs.setup->mode);
    const auto flag=kit::flag::state();
    if(!flag.mode.empty() && flag.mode.starts_with(docs.plugin+"/"))return flag.mode;
    return {};
}
MatchPanelView match_panel_view() {
    MatchPanelView result;
    const auto docs=kit::active();
    if(!docs || !docs->panel.present)return result;
    result.available=true;result.plugin=docs->plugin;result.mode=documents_mode(*docs);
    result.panel=docs->panel;
    result.identity=docs->plugin+"|"+docs->panel.title;
    return result;
}
// Borrow only the selected native activity Page. Never replace Core pause tabs.
struct MatchPanelLease {
    Address manager{};
    Handle generation_page{};
    ActivityDetailsOwnership details;
    ActivityDetailsClose closing;
    Value selection;
    Address page_blueprint{};
    Value shortcut_stack,saved_shortcut,shortcut_content,highlight;
    Value menu_stack,native_page,saved_native_page;
    bool selection_requested{};
    bool selection_consumed{};
    bool selection_retirement_allowed{};
    std::string back_navigation;
    std::string wait_reason;
    std::uint64_t wait_logged{};
};
MatchPanelLease& match_panel_lease(const Context& c) {
    static MatchPanelLease lease;
    if(lease.manager!=c.manager || (lease.generation_page && lease.generation_page!=state().match_panel.page.handle))
        lease={c.manager};
    return lease;
}
void match_panel_wait(const Context& c,std::uint64_t now,const char* reason) {
    if(!state().match_panel.requested_until)return;
    auto& lease=match_panel_lease(c);
    if(lease.wait_reason==reason && now<lease.wait_logged+2000)return;
    lease.wait_reason=reason;lease.wait_logged=now;
    logging::log(logging::Level::warning,logging::Channel::ui,"Throwdowns: requested native details waiting for {}.",reason);
}
bool same_native_model_schema(Address authored,Address runtime,unsigned depth=0) {
    if(authored==runtime)return true;
    if(!authored || !runtime || depth>16 || kind(authored)!=kind(runtime) || size(authored)!=size(runtime))return false;
    const auto a=read<Address>(authored),b=read<Address>(runtime);
    if(read<std::uint32_t>(a)!=read<std::uint32_t>(b))return false;
    if(kind(authored)==4)return same_native_model_schema(read<Address>(a+0x30),read<Address>(b+0x30),depth+1);
    if(kind(authored)!=2)return true;
    const auto count=read<std::uint16_t>(a+0x2a);
    if(count>128 || count!=read<std::uint16_t>(b+0x2a))return false;
    const auto af=read<Address>(a+0x60),bf=read<Address>(b+0x60);
    if(count && (!af || !bf))return false;
    for(unsigned i=0;i<count;++i) {
        const auto x=af+i*24,y=bf+i*24;
        if(read<std::uint32_t>(x)!=read<std::uint32_t>(y) || read<std::uint16_t>(x+8)!=read<std::uint16_t>(y+8) ||
           !same_native_model_schema(read<Address>(x+16),read<Address>(y+16),depth+1))return false;
    }
    return true;
}
Value native_record_model(const Context& c,Address record,std::uint32_t expected_hash) {
    require(record && read<Address>(record+8)==c.base+build::native_menu::asset_record_type,
            "Native activity model record differs.");
    const auto type=read<Address>(record+0x18);
    require(type && kind(type)==2 && read<std::uint32_t>(read<Address>(type))==expected_hash,
            "Native activity model TypeInfo differs.");
    // Resolve in the same manager whose write lock and registry Context owns.
    // The expression wrapper at 0x192ca80 chooses a manager through TLS; its
    // handle can belong to another realm and cannot be read in this registry.
    // Local-profile initialization validates this ABI before installing its
    // record guard. The live entry now contains that detour, so comparing it
    // with the original prologue rejects our own hook. Keep the guard in this
    // call path and use its validated original trampoline.
    require(model_record_load_original!=nullptr,"Native activity record resolver is not initialized.");
    const auto handle=model_record_load_hook(c.manager,record,false);
    if(!handle)return {};
    const auto actual_type=c.type_of(handle);
    if(!same_native_model_schema(type,actual_type)) {
        logging::log(logging::Level::error,logging::Channel::ui,
            "Native activity record {:#x} manager {:#x} returned handle {:#x}: expected TypeInfo {:#x} hash {:#x}, actual {:#x} hash {:#x}.",
            record,c.manager,handle,type,expected_hash,actual_type,
            actual_type?read<std::uint32_t>(read<Address>(actual_type)):0);
        require(false,"Native activity model handle differs from its authored TypeInfo.");
    }
    // The native registry may canonicalize an authored dynamic TypeInfo.
    // Accept only identical reflected schemas and retain the registry's type.
    const Value result{handle,actual_type};c.address(result);return result;
}
Value activity_menu_selection(const Context& c) {
    const auto model=native_record_model(c,asset(c,"Activities_Shared_ContentResource/Activities_SelectedMenu"),0xaf4c67d7);
    return model.handle?c.field(model,0x88391637):Value{};
}
bool request_activity_details(const Context& c) {
    const auto selected=activity_menu_selection(c);
    if(!selected.handle)return false;
    const auto current=read<std::int32_t>(c.address(selected));
    // Rewards, celebrations and other native activity pages keep their owner.
    if(current!=0 && current!=1)return false;
    auto& lease=match_panel_lease(c);
    if(current==1 && !lease.selection_requested)return false;
    lease.selection=selected;lease.details.request();
    if(!current) {
        lease.selection_requested=true;
        lease.selection_consumed=false;
        lease.selection_retirement_allowed=false;
        c.set(selected,std::int32_t{1}); // ActivityMenus.Activities_Details.
        set_custom_activity_menu_request(c.address(selected));
        logging::write(logging::Level::info,logging::Channel::ui,"Throwdowns: native activity menu selected Activities_Details.");
    }
    return true;
}
bool request_native_navigation(const Context& c,const char* action) {
    const auto roots=c.roots({0xcc4776b5});
    if(roots.size()!=1)return false;
    const auto model=roots.front().model;
    require(model.type==c.type({0xcc4776b5,static_cast<std::uint16_t>(size(model.type))}),
            "Native menu navigation TypeInfo differs.");
    const auto navigation=c.field(roots.front().model,0xf3ba9cf8);
    if(read<std::uint32_t>(c.address(navigation)))return false;
    c.array(c.field(roots.front().model,0xf437b255),std::vector<std::int32_t>{-1});
    c.set(navigation,game::native_name_hash(action));
    return true;
}
bool request_native_challenge_menu(const Context& c) {
    // Official Start capture: Activities_OpenPause_Button (0x71760666)
    // writes Activities_SelectedMenu=1, then native Flow_ActivityDetails
    // owns the Page and camera. DelMar_UI_Navigation stays unchanged. Its
    // stale Back=2 after placement previously blocked Start until Map opened.
    return request_activity_details(c);
}
void retire_challenge_request(const Context& c) {
    set_custom_activity_menu_request(0);
    auto& lease=match_panel_lease(c);
    // Expiry withdraws an unmounted request. Completed Back/restoration can
    // also retire our consumed selection only if restoration began while the
    // exact live selected Page/body was still ours. Restoring a retained old
    // Page must never withdraw a replacement stock page's selection.
    if(lease.selection_requested && (!lease.selection_consumed || lease.selection_retirement_allowed)) {
        if(lease.selection.handle && c.type_of(lease.selection.handle)==lease.selection.type &&
           read<std::int32_t>(c.address(lease.selection))==1)c.set(lease.selection,std::int32_t{0});
    }
    lease.selection_requested=false;
}
Value active_match_panel_layer_item(const Context& c,Value layer) {
    const auto list=c.field(layer,items);
    unsigned count{},stride{};c.array(list,32,count,stride);
    if(stride!=stack_item.size)return {};
    const auto selected=active_activity_stack_item(
        read<int>(c.address(c.field(layer,0x49f87835))),count,
        read<bool>(c.address(c.field(layer,0x369babfe))));
    if(!selected)return {};
    const auto item=c.element(list,*selected);
    return read<bool>(c.address(c.field(item,0x369babfe)))?item:Value{};
}
bool match_panel_body_owned(const Context& c,Value native_page) {
    const auto& p=state().match_panel;
    if(!native_page.handle || c.type_of(native_page.handle)!=native_page.type || !p.outer_anchor.handle)return false;
    const auto list=c.field(c.field(native_page,content),items);
    unsigned count{},stride{};c.array(list,32,count,stride);
    if(count!=1 || stride!=stack_item.size)return false;
    const auto item=c.element(list,0);
    const auto display=read<Widget>(c.address(c.field(item,content)));
    return read<int>(c.address(c.field(item,0x1e95752c)))==hud_panel_id+2 &&
        display.blueprint==asset(c,native_menu_detail::anchored_widget) && display.data.record==0 &&
        display.data.handle==p.outer_anchor.handle;
}
bool match_panel_actions_owned(const Context& c,Value native_page) {
    const auto& p=state().match_panel;
    if(!p.quit.handle || c.type_of(p.quit.handle)!=p.quit.type)return false;
    const auto actions=c.field(native_page,0xd2118ca8);
    unsigned count{},stride{};c.array(actions,8,count,stride);
    if(count!=1 || stride!=page_action_schema.size)return false;
    const auto action=c.path(c.element(actions,0),{0xa704272a,0xc52416ef,0xa704272a,0xa2b71c93,0x9c76b86c});
    const auto quit=c.path(p.quit,{0x0fb0d794,0xa704272a,0xc52416ef,0xa704272a,0xa2b71c93,0x9c76b86c});
    const auto expected=read<Address>(c.address(quit));
    return expected && read<Address>(c.address(action))==expected;
}
bool retains_current_match_panel_page(const Context& c) {
    const auto& lease=match_panel_lease(c);const auto& p=state().match_panel;
    if(!lease.details.requested || !lease.details.page || !lease.details.layer || !p.mounted_slot.handle ||
       c.type_of(p.mounted_slot.handle)!=p.mounted_slot.type)return false;
    const auto layer=lease.menu_stack;
    if(!layer.handle || c.type_of(layer.handle)!=layer.type || layer.handle!=lease.details.layer)return false;
    const auto item=active_match_panel_layer_item(c,layer);
    if(!item.handle)return false;
    const auto current=read<Widget>(c.address(c.field(item,content)));
    return current.blueprint==lease.page_blueprint &&
        lease.details.owns(current.data.handle,layer.handle,read<int>(c.address(c.field(item,0x1e95752c)))) &&
        match_panel_body_owned(c,lease.native_page);
}
bool owns_selected_match_panel(const Context& c) {
    const auto& lease=match_panel_lease(c);
    return state().in_menu && lease.selection.handle && c.type_of(lease.selection.handle)==lease.selection.type &&
        read<std::int32_t>(c.address(lease.selection))==1 && retains_current_match_panel_page(c);
}
bool request_menu_toggle(const Context& c) {
    const auto& lease=match_panel_lease(c);
    if(lease.details.requested)
        return owns_selected_match_panel(c) && !lease.back_navigation.empty() &&
            request_native_navigation(c,lease.back_navigation.c_str());
    return false;
}
void detach_match_panel_prompt(const Context& c) {
    state().match_panel.prompt_visible=false;
    auto& lease=match_panel_lease(c);
    if(!lease.shortcut_stack.handle || !lease.saved_shortcut.handle)return;
    if(c.type_of(lease.shortcut_stack.handle)!=lease.shortcut_stack.type) {
        lease.shortcut_stack={};return;
    }
    const auto list=c.field(lease.shortcut_stack,items);
    unsigned count{},stride{};const auto old=c.array(list,32,count,stride);
    require(stride==stack_item.size,"Native Throwdown shortcut stack differs during teardown.");
    const auto type=c.type(stack_item);
    const auto id_offset=c.member(type,0x1e95752c).offset;
    const auto content_offset=c.member(type,content).offset;
    std::vector<std::byte> retained;
    unsigned removed{};
    for(unsigned i=0;i<count;++i) {
        int id{};Widget display{};
        std::memcpy(&id,old.data()+i*stride+id_offset,sizeof(id));
        std::memcpy(&display,old.data()+i*stride+content_offset,sizeof(display));
        if(id==hud_panel_id && display.data.handle==lease.shortcut_content.handle) {++removed;continue;}
        retained.insert(retained.end(),old.begin()+i*stride,old.begin()+(i+1)*stride);
    }
    if(removed) {
        if(removed==count) {
            // Installation only borrows an empty authored shortcut stack.
            // Restore it once while its sole mounted item is still ours.
            c.copy(lease.shortcut_stack,c.address(lease.saved_shortcut));
        } else {
            // Native UI has since mounted another shortcut. Remove only our
            // item; leave that newer owner's active/focus/transition fields.
            // Context::array retains the source snapshot in a transient typed
            // owner before changing the destination's counted native array.
            c.array(list,retained,static_cast<unsigned>(retained.size()/stride));
        }
    }
    lease.shortcut_stack={};
}
void restore_match_panel_mount(const Context& c) {
    auto& s=state();auto& p=s.match_panel;
    auto& lease=match_panel_lease(c);
    const bool owned_selection=owns_selected_match_panel(c);
    // StackScreenController's destructor finds its item by the original Page
    // DataContext/Blueprint. Keep that identity intact throughout native Back;
    // restore only the body/actions we borrowed, even after the item is gone.
    if(lease.saved_native_page.handle && lease.native_page.handle &&
       c.type_of(lease.native_page.handle)==lease.native_page.type &&
       c.type_of(lease.saved_native_page.handle)==lease.saved_native_page.type) {
        if(match_panel_body_owned(c,lease.native_page)) {
            c.copy(c.field(lease.native_page,content),c.address(c.field(lease.saved_native_page,content)));
            lease.selection_retirement_allowed=owned_selection;
        }
        if(match_panel_actions_owned(c,lease.native_page))
            c.copy(c.field(lease.native_page,0xd2118ca8),c.address(c.field(lease.saved_native_page,0xd2118ca8)));
    }
    lease.menu_stack={};lease.native_page={};lease.saved_native_page={};
    p.mounted_slot={};s.match_panel_open.store(false,std::memory_order_release);
    overlay::capture_challenge_menu_input(match_panel_view().available,false,s.panel_key_down);
    if(s.in_menu || p.quitting || !match_panel_view().available)detach_match_panel_prompt(c);
    lease.details.clear_mount();lease.back_navigation.clear();lease.page_blueprint=0;
}
void restore_match_panel(const Context& c) {
    restore_match_panel_mount(c);
    retire_challenge_request(c);
    auto& lease=match_panel_lease(c);lease.details.retire();lease.selection={};
    lease.closing.reset();
    auto& p=state().match_panel;p.requested_until=0;p.request_after=0;
}
bool close_match_panel(const Context& c,std::uint64_t now) {
    auto& lease=match_panel_lease(c);
    lease.closing.begin(now);
    if(!owns_selected_match_panel(c)) {
        // Native Back has completed, or another owner replaced this page.
        // Restore only cached copies still retaining our exact presenter.
        restore_match_panel(c);
        logging::write(logging::Level::info,logging::Channel::ui,
            "Throwdowns: native details close completed; owned body/actions restored and input released.");
        return true;
    }
    if(lease.closing.needs_back()) {
        const bool queued=request_menu_toggle(c);
        lease.closing.back_queued(queued);
        if(queued)logging::write(logging::Level::info,logging::Channel::ui,
            "Throwdowns: native details Back queued; retaining ownership until native teardown.");
    }
    // A stale busy navigation action must not strand a completed match. The
    // captured native Back clears Activities_SelectedMenu to zero. Recover
    // that selection only while this exact private page is still selected and
    // we own the original 0->1 write; never touch a replacement stock page.
    const bool own_selection=lease.selection_requested && lease.selection.handle &&
        c.type_of(lease.selection.handle)==lease.selection.type &&
        read<std::int32_t>(c.address(lease.selection))==1;
    if(lease.closing.needs_selection_recovery(now,true,own_selection)) {
        c.set(lease.selection,std::int32_t{0});
        logging::write(logging::Level::warning,logging::Channel::ui,
            "Throwdowns: native Back did not consume the owned details selection; withdrawing that exact selection.");
    }
    return false;
}
bool pending_match_panel_request(const Context&,std::uint64_t now) {
    const auto& p=state().match_panel;
    return p.requested_until && now<p.requested_until;
}
std::optional<float> native_activity_camera_fov(const Context& c) {
    const auto manager=read<Address>(c.base+0x75cbc48);
    if(!manager || read<Address>(manager)!=c.base+0x63ece68)return {};
    const auto first=read<Address>(manager+8),last=read<Address>(manager+16);
    if(last<first || last-first>32*8 || (last-first)%8)return {};
    for(auto at=first;at<last;at+=8) {
        const auto view=read<Address>(at);
        if(!view || read<Address>(view)!=c.base+0x63ed0a0 || read<unsigned>(view+0x148) || read<unsigned>(view+0x14c))continue;
        const auto camera=read<Address>(view+0x2b0);
        if(!camera)return {};
        const auto fov=read<float>(camera+0xac);
        if(std::isfinite(fov) && fov>1.f && fov<180.f)return fov;
    }
    return {};
}
Value panel_stack_item(const Context& c,Value stack,Widget display,int id) {
    const auto item=make(c,stack_item);
    c.set(c.field(item,content),display);c.set(c.field(item,0x1e95752c),id);
    c.copy(c.field(item,0xd78d240b),c.address(c.field(stack,0x54c9abee)));
    c.set(c.field(item,0x369babfe),true);
    for(const auto track:{0x8c0583eeU,0xc5c7cc11U}) {
        c.set(c.path(item,{track,0x20247753}),1.f);
        c.set(c.path(item,{track,0x2426103f}),1.f);
        c.set(c.path(item,{track,0x1e3ce013}),true);
    }
    return item;
}
void panel_stack_content(const Context& c,Value stack,Widget display,int id) {
    const auto item=panel_stack_item(c,stack,display,id);
    std::vector<std::byte> bytes(stack_item.size);
    require(memory::peek_bytes(c.address(item),bytes.data(),bytes.size()),"Native Throwdown Panel content unavailable.");
    c.array(c.field(stack,items),bytes,1);
    c.set(c.field(stack,0x369babfe),true);
    c.set(c.field(stack,0xc912aa1d),false);
    c.set(c.field(stack,0x2a98baa3),false);
    c.set(c.field(stack,0x18f8355b),0);
    c.set(c.field(stack,0x55511280),0);
}
// The prompts under the HUD while a match runs: the document's own (visual only; their inputs are
// sampled for panel_command) and the native "Throwdown Panel" prompt last.
bool mount_match_panel_prompt(const Context& c) {
    auto& s=state();auto& p=s.match_panel;auto& lease=match_panel_lease(c);
    if(s.in_menu) {detach_match_panel_prompt(c);return false;}
    // This is a persistent visual prompt. A StackScreen with an empty focus
    // target treats it as a dismissed shortcut and hides it after its intro.
    // Present the native LabelButton directly; Start still belongs to the
    // separately verified challenge flow, never this non-clickable prompt.
    require(s.hud_root.handle && c.type_of(s.hud_root.handle)==s.hud_root.type,
            "Native gameplay shortcut HUD is not mounted.");
    if(lease.shortcut_stack.handle)detach_match_panel_prompt(c);
    const bool fresh=!p.prompt_item.handle;
    const auto button_widget=asset(c,"UI/Foundations/Components/Buttons/LabelButton/LabelButton_Widget");
    const unsigned rows=p.extra_count+1;
    constexpr float width=native_hud_shortcut_width;
    if(p.prompt_rows!=rows || !hud_list_has_items(c,p.prompt_list,rows)) {
        std::vector<Widget> controls;
        for(unsigned i=0;i<p.extra_count;++i)controls.push_back({button_widget,{0,p.extra_prompts[i].handle}});
        controls.push_back({button_widget,{0,p.prompt.handle}});
        hud_list(c,p.prompt_list,controls,false,88.f,8.f,0.f,width);
        const auto cells=c.field(p.prompt_list,0xf2c90867);
        for(unsigned i=0;i<rows;++i){const auto handle=read<Handle>(c.address(c.element(cells,i)));const Value row{handle,c.type_of(handle)};
            c.set_if_changed(c.field(row,0x1cff7243),std::array<float,2>{width,88.f});
            for(const auto key:{0x99bbed5aU,0x49943bedU,0xfec5e0b1U})c.set_if_changed(c.path(row,{0xde7d30c7,key}),0.f);
        }
        p.prompt_rows=rows;
    }
    mount_hud(c,p.prompt_list,asset(c,"UI/Foundations/Components/Lists/LinearList/Widget/LinearList_Widget"),p.prompt_anchor,p.prompt_item,hud_panel_id,false);
    c.set_if_changed(c.field(p.prompt_anchor,0x1cff7243),std::array<float,2>{width,96.f*float(rows)-8.f});
    for(const auto axis:{0xde7d30c7U,0x185a5736U}) {
        const float at=axis==0xde7d30c7?native_hud_screen_anchor(native_hud_shortcut_edge):native_hud_screen_anchor(.008f);
        for(const auto field:{0x99bbed5aU,0x49943bedU})c.set_if_changed(c.path(p.prompt_anchor,{axis,field}),at);
        for(const auto field:{0x40bfbff4U,0x1d233266U})c.set_if_changed(c.path(p.prompt_anchor,{axis,field}),0.f);
        c.set_if_changed(c.path(p.prompt_anchor,{axis,0xfec5e0b1}),axis==0xde7d30c7?1.f:0.f);
        c.set_if_changed(c.path(p.prompt_anchor,{axis,0xd01f4a21}),1.f);
    }
    p.prompt_visible=true;
    if(fresh)logging::write(logging::Level::info,logging::Channel::ui,
        "Throwdowns: native Activity pause control mounted on the visible custom HUD.");
    return true;
}
Value match_panel_input_action(const Context& c,Value button_value) {
    return c.path(button_value,{0x0fb0d794,0xa704272a,0xc52416ef,0xa704272a,0xa2b71c93});
}
void match_panel_input(const Context& c,Value button_value,Address input_asset,bool prompt) {
    require(input_asset!=0,"Native Throwdown Panel input unavailable.");
    const auto action=match_panel_input_action(c,button_value);
    // The native glyph and shortcut resolve different input fields. Supplying
    // both keeps the controller glyph and keyboard binding on the same action.
    for(const auto key:{0x3d00de27U,0x0169c2dbU}) {
        const auto input_field=c.field(action,key);
        if(read<Address>(c.address(input_field))!=input_asset)c.set(input_field,input_asset);
    }
    const auto interactable=c.path(button_value,{0x0fb0d794,0xa704272a,0xc52416ef,0xa704272a});
    const auto is_prompt=c.field(interactable,0x38f7355f);
    if(read<bool>(c.address(is_prompt))!=prompt)c.set(is_prompt,prompt);
    const auto clickable=c.field(interactable,0x4b3146e9);
    if(read<bool>(c.address(clickable))==prompt)c.set(clickable,!prompt);
}
void match_panel_action(const Context& c,Value button_value,const std::string& caption,const MatchPanelView& view,bool quit) {
    plain_label(c,c.field(button_value,0x58c9d354),caption);
    const auto action=match_panel_input_action(c,button_value);
    const auto delegate=c.field(action,0x9c76b86c);
    const auto target=callback(c,quit?"panel-quit":"panel-open",view.identity);
    // Keep the installed handler stable while held/pressed state is running.
    // Calling callback still retains the descriptor for this UI generation.
    if(read<Address>(c.address(delegate))!=target)c.set(delegate,target);
    const auto navigation=c.field(action,0xb1e14cd6);
    if(!c.text(navigation).empty())c.text(navigation,"");
}
std::string quit_caption(const MatchPanelView& view) { return view.panel.quit.empty()?std::string("QUIT"):view.panel.quit; }
void update_match_panel_quit(const Context& c,Value page_value,Value quit,const MatchPanelView& view) {
    match_panel_action(c,quit,quit_caption(view),view,true);
    const auto actions=c.field(page_value,0xd2118ca8);
    unsigned count{},stride{};c.array(actions,8,count,stride);
    require(count==1 && stride==page_action_schema.size,"Native Throwdown Panel Quit shortcut differs.");
    const auto mounted=c.path(c.element(actions,0),{0xa704272a,0xc52416ef,0xa704272a,0xa2b71c93});
    const auto source=match_panel_input_action(c,quit);
    const auto page_action=c.element(actions,0);
    const auto bar=c.field(page_action,0xb6e9bfa0); // Native generated ButtonBarLabelButton context handle.
    if(size(bar.type)==sizeof(Handle)) {
        const auto handle=read<Handle>(c.address(bar));
        if(handle && game::native_data().models.value(c.manager,handle,0,1)) {
            const Value hint{handle,c.type_of(handle)};
            if(hint.type==c.type(button))plain_label(c,c.field(hint,0x58c9d354),quit_caption(view));
        }
    }

    if(read<Address>(c.address(c.field(mounted,0x9c76b86c)))!=read<Address>(c.address(c.field(source,0x9c76b86c))))
        c.copy(mounted,c.address(source));
}
void match_panel_notice_width(const Context& c,Value notice,float width) {
    // LabelWidthConstraint is two optional floats, not a scalar. Match the
    // reflected native schema so narrowing a cell cannot abort HUD building.
    const auto constraint=c.field(notice,0xd369e2db);
    c.set_if_changed(c.path(constraint,{0x3ccf8bcd,0x67800332}),false);
    c.set_if_changed(c.path(constraint,{0x48269971,0x2358a817}),width);
    c.set_if_changed(c.path(constraint,{0x48269971,0x67800332}),true);
}
void match_panel_table_style(const Context& c,Value notice) {
    const auto slot=c.field(notice,0x8cc042ef);
    const auto current=read<Ref>(c.address(slot));
    if(!current.record)return; // Already a private style for this cell.
    const auto source=reference(c,current);
    require(source.type && size(source.type)==152,"Native notice table style differs.");
    const auto style=make(c,{0x3870cb1b,152},source.type);c.copy(style,c.address(source));
    // Custom frames already include their text inset. The inherited native
    // Notice adds 24px on both sides, reducing a 100px position cell to a
    // sliver and giving differently sized headings/rows different centers.
    for(const auto padding:{0x8ca12548U,0xffaf82ffU,0x4a8068ceU})
        for(const auto edge:{0xd7b53526U,0x29023866U,0xcb57674fU,0xf750f0a2U})
            c.set(c.path(style,{padding,edge}),0);
    c.set(c.field(style,3705201444U),false); // ShouldAnimate: a table has a single parent transition.
    c.set(slot,Ref{0,style.handle});
}
void match_panel_list_size(const Context& c,Value list,unsigned index,float width,float height,float y=0.f) {
    const auto handle=read<Handle>(c.address(c.element(c.field(list,0xf2c90867),index)));
    const Value anchor{handle,c.type_of(handle)};
    c.set_if_changed(c.field(anchor,0x1cff7243),std::array<float,2>{width,height});
    c.set_if_changed(c.field(anchor,0x6efc1a61),false);
    c.set_if_changed(c.field(anchor,0xbe5683d9),false);
    for(const auto axis:{0xde7d30c7U,0x185a5736U}) {
        c.set_if_changed(c.path(anchor,{axis,0xd01f4a21}),1.f);
        for(const auto field:{0x40bfbff4U,0x1d233266U})
            c.set_if_changed(c.path(anchor,{axis,field}),axis==0x185a5736U?y:0.f);
    }
}
// The rules notice and the first `count` setting rows (up to three).
void match_panel_settings(const Context& c,unsigned count) {
    auto& p=state().match_panel;
    std::vector<Widget> settings{{asset(c,"UI/Foundations/Components/Notices/BasicNotice_Widget"),{0,p.notice.handle}}};
    for(unsigned i=0;i<count && i<p.setting_rows.size();++i)
        settings.push_back({asset(c,"UI/Foundations/Components/Settings/Widgets/SettingWrapper_Widget"),{0,p.setting_rows[i].handle}});
    hud_list(c,p.settings,settings,false,144.f,16.f,0.f,1120.f);
    const auto rules_anchor=read<Handle>(c.address(c.element(c.field(p.settings,0xf2c90867),0)));
    c.set(c.field({rules_anchor,c.type_of(rules_anchor)},0x1cff7243),std::array<float,2>{1120.f,320.f});
    p.setting_count=count;
}
// A setting's icon: players, timer, rounds (skate.'s own), or a texture path in white.
void match_panel_setting_icon(const Context& c,unsigned i,const std::string& icon) {
    auto& p=state().match_panel;
    const auto name=icon.empty()?std::string(i==0?"players":i==1?"timer":"rounds"):icon;
    if(p.setting_icon_names[i]==name)return;
    try {
        const auto stock=name=="players"?"UI/Textures/Icons/img_Icon_PlayerGroup_64":name=="timer"?"UI/Textures/Icons/Generic/img_Icon_TimerOutline_64":
            name=="rounds"?"UI/Textures/Icons/Generic/img_Icon_Rounds_128":nullptr;
        if(stock) {
            c.set(c.field(p.setting_icons[i],0x6e469d2a),asset(c,stock));
            c.set(c.field(p.setting_icons[i],0xcd4a53c0),record_ref(c,"InteractivityColorStylesList/InteractiveColors.Default.Darkest"));
        } else {
            mode_art(c,std::array{c.field(p.setting_icons[i],0x6e469d2a)},name);
            c.set_if_changed(c.field(p.setting_icons[i],0xcd4a53c0),record_ref(c,"InteractivityColorStylesList/InteractiveColors.Core.AllWhite"));
        }
        p.setting_icon_names[i]=name;
    } catch(const std::exception& e) {
        // Optional decoration must never prevent the native Quit page
        // from opening if a map is still making an icon resident.
        static std::set<std::string> reported;
        if(reported.insert(name).second)logging::log(logging::Level::warning,logging::Channel::ui,"Throwdown setting icon deferred: {}",e.what());
    }
}
void build_match_panel(const Context& c,const MatchPanelView& view) {
    auto& p=state().match_panel;if(p.page.handle)return;
    const auto checkpoint=state().owned.checkpoint();
    try {
    // Copy the complete mutable input graph once. Backend styles and immutable
    // assets stay borrowed; no stock mode's model is changed by this page.
    const auto page_value=record_model(c,"Throwdown_DetailsMenu_ContentResource/Throwdowns_Details_Page",page);
    PrivateUiGraph graph{c};graph.walk(page_value,0);
    p.panel=record_model(c,"Throwdown_DetailsMenu_ContentResource/Throwdowns_Details_StackPanel",panel_schema);
    graph.walk(p.panel,0);
    p.header=record_model(c,"Throwdown_DetailsMenu_ContentResource/Throwdown_Details_StackPanel_HeaderContent",header_schema);
    graph.walk(p.header,0);
    plain_label(c,c.field(p.header,0x44688629),"Throwdown");
    plain_label(c,c.field(p.header,0x00f3b15e),view.panel.title);
    label_styles(c,c.field(p.header,0x44688629),"TextStylesList/B52-SemiB_Bright","TextStylesList/B52-SemiB_Bright");
    label_styles(c,c.field(p.header,0x00f3b15e),"TextStylesList/H72-XBold_White","TextStylesList/H72-XBold_White");
    c.set(c.path(p.header,{0x00f3b15e,0xfc53d427}),false);
    c.set(c.field(p.header,0x2840ecf6),Widget{});
    p.badge=make(c,texture_schema);
    c.set(c.field(p.badge,0xf524c836),192.f);c.set(c.field(p.badge,0x6fbd254e),192.f);
    const auto header_label_widget=asset(c,"UI/Foundations/Components/Text/Label/Widget/Label_Widget");
    const auto list_widget=asset(c,"UI/Foundations/Components/Lists/LinearList/Widget/LinearList_Widget");
    for(const auto key:{0x44688629U,0x00f3b15eU}) {
        const auto label=c.field(p.header,key);
        c.set(c.field(label,0xc13ba637),false);c.set(c.field(label,0xfc53d427),false);
        c.set(c.field(label,0x3d8639d0),0);
    }
    // StackPanel measures its header by the child widget's intrinsic height.
    // A full-screen AnchoredContentPresenter reports a full-screen height,
    // regardless of its inner DesiredSize. Mount the measured list directly.
    hud_list(c,p.header_titles,{{header_label_widget,{0,c.field(p.header,0x44688629).handle}},
        {header_label_widget,{0,c.field(p.header,0x00f3b15e).handle}}},false,88.f,8.f,0.f,760.f);
    hud_list(c,p.header_rows,{{}, {asset(c,"UI/ReSkate/Throwdowns/LogoTexture_Widget"),{0,p.badge.handle}},
        {list_widget,{0,p.header_titles.handle}}},true,192.f,32.f);
    match_panel_list_size(c,p.header_rows,0,32.f,192.f);
    match_panel_list_size(c,p.header_rows,1,192.f,192.f);
    match_panel_list_size(c,p.header_rows,2,760.f,192.f);
    c.set(c.field(p.panel,0xc0f1b449),Widget{list_widget,{0,p.header_rows.handle}});
    p.notice=record_model(c,"TD_Page_ContentResources/TD_Notice_MenuDescriptionTitle",notice_schema);
    plain_label(c,c.field(p.notice,0x94703efc),view.panel.description);
    c.set(c.field(p.notice,0x13be0d51),std::array<float,2>{1120.f,320.f});
    label_styles(c,c.field(p.notice,0x94703efc),"TextStylesList/B52-SemiB_Bright","TextStylesList/B52-SemiB_Bright");
    c.set(c.path(p.notice,{0x94703efc,0xfc53d427}),true);
    match_panel_notice_width(c,p.notice,1056.f);
    c.set(c.field(p.panel,0x4fd7853d),Widget{});
    for(unsigned i=0;i<p.setting_rows.size();++i) {
        p.setting_rows[i]=make(c,setting_schema);
        c.set(c.field(p.setting_rows[i],0xc9bf367d),record_ref(c,"SettingWrapperStylesList/Horizontal.WithDivider"));
        c.set(c.field(p.setting_rows[i],0xf3d74cdd),0);
        const auto caption=c.field(p.setting_rows[i],0x58c9d354);
        c.copy(caption,c.address(state().source_label));
        plain_label(c,caption,"");
        label_styles(c,caption,"TextStylesList/B52-SemiB_Black","TextStylesList/B52-SemiB_Black");
        p.setting_titles[i]=make(c,label_schema);
        c.copy(p.setting_titles[i],c.address(state().source_label));
        plain_label(c,p.setting_titles[i],"");
        label_styles(c,p.setting_titles[i],"TextStylesList/B52-SemiB_Black","TextStylesList/B52-SemiB_Black");
        c.set(c.field(p.setting_titles[i],0xc13ba637),false);
        c.set(c.field(p.setting_titles[i],0xfc53d427),false);
        c.set(c.field(p.setting_titles[i],0x3d8639d0),0);
        p.setting_values[i]=make(c,label_schema);
        c.copy(p.setting_values[i],c.address(state().source_label));
        c.set(c.field(p.setting_values[i],0xc13ba637),false);
        c.set(c.field(p.setting_values[i],0xfc53d427),false);
        label_styles(c,p.setting_values[i],"TextStylesList/H64-XBold_Black","TextStylesList/H64-XBold_Black");
        c.set(c.field(p.setting_values[i],0x3d8639d0),1);
        p.setting_icons[i]=make(c,texture_schema);
        c.set(c.field(p.setting_icons[i],0xf524c836),64.f);
        c.set(c.field(p.setting_icons[i],0x6fbd254e),64.f);
        for(const auto axis:{0xde7d30c7U,0x185a5736U}) {
            const bool vertical=axis==0x185a5736U;
            for(const auto edge:{0x99bbed5aU,0x49943bedU})
                c.set(c.path(p.setting_icons[i],{axis,edge}),vertical?.5f:0.f);
            c.set(c.path(p.setting_icons[i],{axis,0xfec5e0b1}),vertical?.5f:0.f);
            c.set(c.path(p.setting_icons[i],{axis,0xd01f4a21}),1.f);
        }
        const auto label_widget=asset(c,"UI/Foundations/Components/Text/Label/Widget/Label_Widget");
        hud_list(c,p.setting_lists[i],{{asset(c,"UI/Foundations/Components/Media/Icons/Texture_Widget"),{0,p.setting_icons[i].handle}},
            {label_widget,{0,p.setting_titles[i].handle}},{label_widget,{0,p.setting_values[i].handle}}},true,112.f,24.f);
        match_panel_list_size(c,p.setting_lists[i],0,80.f,112.f);
        match_panel_list_size(c,p.setting_lists[i],1,512.f,112.f);
        match_panel_list_size(c,p.setting_lists[i],2,320.f,112.f);
        p.setting_anchors[i]=make(c,anchor_schema);
        c.set(c.field(p.setting_anchors[i],widget),Widget{asset(c,"UI/Foundations/Components/Lists/LinearList/Widget/LinearList_Widget"),{0,p.setting_lists[i].handle}});
        c.set(c.field(p.setting_anchors[i],0x1cff7243),std::array<float,2>{960.f,112.f});
        for(const auto axis:{0xde7d30c7U,0x185a5736U}) {
            const bool vertical=axis==0x185a5736U;
            for(const auto edge:{0x99bbed5aU,0x49943bedU})c.set(c.path(p.setting_anchors[i],{axis,edge}),0.f);
            c.set(c.path(p.setting_anchors[i],{axis,0xfec5e0b1}),0.f);
            c.set(c.path(p.setting_anchors[i],{axis,0xd01f4a21}),1.f);
            for(const auto edge:{0x40bfbff4U,0x1d233266U})c.set(c.path(p.setting_anchors[i],{axis,edge}),vertical?0.f:64.f);
        }
        c.set(c.field(p.setting_rows[i],content),Widget{asset(c,native_menu_detail::anchored_widget),{0,p.setting_anchors[i].handle}});
    }
    match_panel_settings(c,static_cast<unsigned>(p.setting_rows.size()));
    panel_stack_content(c,c.field(p.panel,0x43967ece),
        {asset(c,"UI/Foundations/Components/Lists/LinearList/Widget/LinearList_Widget"),{0,p.settings.handle}},hud_panel_id+1);
    // This authored footer is an empty list; the stock challenge rule supplies
    // its buttons later. Instantiate the separately exported native Quit button
    // instead of looking for a button that is absent from the template graph.
    // Resolve the exported footer list and anchor independently. The panel's
    // inline footer graph contains ContentWithTitle and presenter wrappers;
    // treating its first referenced model as an anchor asks for a field it
    // does not have and aborts construction before a Page can be mounted.
    const auto footer_list=record_model(c,"Throwdown_DetailsMenu_ContentResource/Throwdowns_Details_StackPanel_FooterContent",linear_list);
    const auto footer_anchor=record_model(c,"Throwdown_DetailsMenu_ContentResource/Throwdowns_Details_StackPanel_Footer_AnchoredContentPresenter",anchor_schema);
    graph.walk(footer_list,0);graph.walk(footer_anchor,0);
    c.set(c.field(footer_anchor,widget),Widget{asset(c,"UI/Foundations/Components/Lists/LinearList/Widget/LinearList_Widget"),{0,footer_list.handle}});
    c.set(c.field(p.panel,0xbfc64535),Widget{asset(c,native_menu_detail::anchored_widget),{0,footer_anchor.handle}});
    p.quit=record_model(c,"Throwdown_DetailsMenu_ContentResource/Throwdowns_Details_Quit_Button",button);
    graph.walk(p.quit,0);
    clear_value(c,c.field(p.quit,0x67212c85));
    // The donor Quit focus predicate queries its native challenge backend. This
    // private page validates availability through our own match coordinator.
    clear_value(c,c.path(p.quit,{0x0fb0d794,0xa704272a,0xc52416ef,0x66a997b9,0x66a997b9}));
    c.set(c.field(p.quit,0x659db23e),false);
    const auto quit_input=read<Address>(c.address(c.field(match_panel_input_action(c,p.quit),0x0169c2db)));
    match_panel_input(c,p.quit,quit_input,false);
    c.copy(c.field(p.quit,0x58c9d354),c.address(state().source_label));
    label_styles(c,c.field(p.quit,0x58c9d354),"TextStylesList/B52-SemiB-Caps_Dark","TextStylesList/B52-SemiB_Black");
    c.set(c.path(p.quit,{0x58c9d354,0x3d8639d0}),0);
    match_panel_action(c,p.quit,quit_caption(view),view,true);
    c.array(c.field(footer_list,0xf2c90867),std::vector<Handle>{});
    c.array(c.field(footer_list,rows_field),std::vector<Ref>{{0,p.quit.handle}});
    // A native PageAction keeps Quit available while a leaderboard row has
    // focus, just as the original details page does for its shortcut controls.
    const auto page_actions=c.field(page_value,0xd2118ca8);
    const auto action_type=read<Address>(read<Address>(page_actions.type)+0x30);
    const auto quit_action=make(c,page_action_schema,action_type);
    c.copy(c.field(quit_action,0xa704272a),c.address(c.path(p.quit,{0x0fb0d794,0xa704272a})));
    c.text(c.field(quit_action,0x6c0c58da),"Quit"); // PageAction.FilterKey is required by the button bar.
    c.set(c.field(quit_action,0xb3da8457),true); // CanClick.

    std::vector<std::byte> action_bytes(page_action_schema.size);
    require(memory::peek_bytes(c.address(quit_action),action_bytes.data(),action_bytes.size()),
            "Native Throwdown Panel Quit shortcut unavailable.");
    c.array(page_actions,action_bytes,1);
    p.board=record_model(c,"TD_Scoreboard_ContentResources/LEGACY_TD_Scoreboard");
    graph.walk(p.board,0);
    // LEGACY_TD_Scoreboard defaults to HUDLeaderboardListElement, whose input
    // schema is different. It renders its sample stamps/999999 Pts when given
    // a PlayerInlineScoreboardViewModel. Use that model's actual list bridge.
    c.set(c.path(p.board,{0xfcdd0d32,0x55ca89f4}),
        asset(c,"UI/Features/CommunityEvent/LEGACY_PlayerInlineScoreboardList_Widget"));
    p.columns=make(c,linear_list);
    p.outer_anchor=record_model(c,"Throwdown_DetailsMenu_ContentResource/Throwdowns_Details_AnchoredContentPresenter",anchor_schema);
    c.set(c.field(p.outer_anchor,widget),Widget{asset(c,"UI/Foundations/Components/Lists/LinearList/Widget/LinearList_Widget"),{0,p.columns.handle}});
    panel_stack_content(c,c.field(page_value,content),
        {asset(c,native_menu_detail::anchored_widget),{0,p.outer_anchor.handle}},hud_panel_id+2);
    const auto back_action=back(c,page_value);
    clear_value(c,c.field(back_action,0x9c76b86c));
    c.text(c.field(back_action,0xb1e14cd6),"ThrowdownDetailsBack");
    const auto back_input=asset(c,"Configurations/Input/UI/UI_Back");
    c.set(c.field(back_action,0x3d00de27),back_input);c.set(c.field(back_action,0x0169c2db),back_input);
    p.page=page_value; // Publish the completed page only after every edge exists.
    match_panel_lease(c).generation_page=p.page.handle;
    logging::log(logging::Level::info,logging::Channel::ui,"{}: private native Throwdown details Page, Quit and scoreboard constructed.",view.panel.title);
    } catch(...) {
        const auto prompt=p.prompt,prompt_anchor=p.prompt_anchor,prompt_item=p.prompt_item;
        const auto requested=p.requested_until;
        const auto menu_was_open=p.menu_was_open;
        p={};p.prompt=prompt;p.prompt_anchor=prompt_anchor;p.prompt_item=prompt_item;p.requested_until=requested;
        p.menu_was_open=menu_was_open;
        state().owned.rollback(checkpoint,[&](Value value){c.destroy(value);});
        throw;
    }
}
void refresh_match_panel(const Context& c,const MatchPanelView& view) {
    auto& p=state().match_panel;auto& lease=match_panel_lease(c);const auto& panel=view.panel;
    plain_label(c,c.field(p.header,0x00f3b15e),panel.title);
    plain_label(c,c.field(p.notice,0x94703efc),panel.description);
    mode_art(c,std::array{c.field(p.badge,0x6e469d2a)},panel.art);
    const auto settings=static_cast<unsigned>(std::min<std::size_t>(panel.settings.size(),p.setting_rows.size()));
    if(settings!=p.setting_count)match_panel_settings(c,settings);
    for(unsigned i=0;i<settings;++i) {
        plain_label(c,p.setting_titles[i],panel.settings[i].caption);
        plain_label(c,p.setting_values[i],panel.settings[i].value);
        match_panel_setting_icon(c,i,panel.settings[i].icon);
    }
    const auto action_page=match_panel_body_owned(c,lease.native_page)?lease.native_page:p.page;
    update_match_panel_quit(c,action_page,p.quit,view);
    // A row shows its value, and its outcome beside it.
    const auto row_text=[](const kit::PanelPlayer& player) {
        return player.outcome.empty()?player.value:player.value.empty()?player.outcome:player.value+"  "+player.outcome;
    };
    std::array<float,3> widths{200.f,640.f,400.f};
    for(const auto& row:panel.players) {
        widths[1]=std::max(widths[1],float(native_hud_name_width(row.name)));
        widths[2]=std::max(widths[2],1.5f*float(native_hud_value_width(row_text(row))));
    }
    const float width=widths[0]+widths[1]+widths[2]+16.f;
    const bool resized=p.board_widths!=widths;
    // Reuse the game's complete Throwdown scoreboard, profile rows, rank
    // badges and local-player treatment. Its own template aligns the column
    // headings and rows; separate notice boxes gave it a foreign appearance.
    c.set_if_changed(c.field(p.board,0x5a7cec85),true);
    c.set_if_changed(c.field(p.board,0xb7e92e95),true);
    const auto headers=c.field(p.board,0x3d98d397);
    for(unsigned col=0;col<3;++col) {
        const auto label=c.element(headers,col);
        plain_label(c,label,col==0?"POS":col==1?"NAME":panel.score_caption);
        label_styles(c,label,"TextStylesList/H52-XBold_White","TextStylesList/H52-XBold_White");
    }
    std::vector<Ref> rows;std::vector<std::uint64_t> order;unsigned rank{};
    for(const auto& player:panel.players) {
        auto& row=p.players[player.id];++rank;
        if(!row.row.handle) {
            row.row=record_model(c,"TD_Scoreboard_ContentResources/TD_PlayerProfileInline");
            PrivateUiGraph graph{c};graph.walk(row.row,0);
            row.rank=record_model(c,"TD_Scoreboard_ContentResources/TD_DefaultRank");
            row.outcome=record_model(c,"TD_Hud_ContentResources/TD_HUD_Header_Label");
            auto display=read<Widget>(c.address(c.field(row.row,0xc76d8b73)));
            display.data={0,row.rank.handle};c.set(c.field(row.row,0xc76d8b73),display);
            const auto scores=c.field(row.row,0xa3d2bba9);
            // The donor score list presents S.K.A.T.E. stamps. Supply a label
            // template AND live handles, rather than inheriting those defaults.
            c.set(c.field(scores,0x55ca89f4),asset(c,"UI/Foundations/Components/Text/Label/Widget/LabelListItem_Widget"));
            c.array(c.field(scores,rows_field),std::vector<Ref>{});
            c.array(c.field(scores,0xf2c90867),std::vector<Handle>{row.outcome.handle});
            label_styles(c,row.outcome,"TextStylesList/B52-SemiB_Bright","TextStylesList/B52-SemiB_Bright");
            c.set(c.field(row.outcome,0xfc53d427),false);
            c.set(c.field(row.outcome,0xc13ba637),false);
        }
        c.set_if_changed(c.field(row.row,0x8810a01e),player.self);
        const auto name=c.path(row.row,{0xaed127c3,profile_name_tag_field});
        c.set_if_changed(c.field(name,0xc13ba637),false);
        c.set_if_changed(c.field(name,0xfc53d427),false);
        label_styles(c,name,"TextStylesList/H52-XBold_White","TextStylesList/H52-XBold_White");
        plain_label(c,name,player.name);
        // Use only this account's cached picture for this account's row.
        // Optional portrait loading cannot prevent the native Quit panel.
        if(!row.portrait_attempted)try {
            const auto record=asset(c,("Throwdown_ModeSelect_ContentResources/SteamAvatar_"+std::to_string(player.id)).c_str());
            const auto type=native_record_type(c,record);
            require(read<std::uint32_t>(read<Address>(type))==category_schema.hash,"Steam picture record schema differs.");
            const auto image=read<Address>(read<Address>(record+0x20)+c.member(type,0x189084da).offset);
            require(image && (read<Address>(image+8)==c.base+build::native_menu::texture_asset_type ||
                read<Address>(image+8)==c.base+build::native_menu::image_asset_type),"Steam picture is not resident yet.");
            for(const auto texture:ui_models(c,c.field(row.row,0xaed127c3),2005356313U)) {
                c.set_if_changed(c.field(texture,1888565495U),image);
                c.text(c.field(texture,3630279999U),"");
                row.portrait_attempted=true;
            }
        } catch(const std::exception&) { /* Other players keep their own native portrait. */ }
        // The document's picture through the row's own native web image: the player's Steam
        // picture ("steam"), or one on Steam's image servers.
        if(!row.portrait_attempted && !player.portrait.empty()) {
            const auto url=player.portrait=="steam"?steam_picture(player.id):steam_image_url(player.portrait)?player.portrait:std::string{};
            if(!url.empty() && url!=row.picture_url) {
                for(const auto texture:ui_models(c,c.field(row.row,0xaed127c3),2005356313U))c.text(c.field(texture,3630279999U),url);
                row.picture_url=url;
            }
        }
        plain_label(c,c.field(row.rank,0x2718c842),player.rank.empty()?std::to_string(rank):player.rank);
        plain_label(c,row.outcome,row_text(player));
        order.push_back(player.id);rows.push_back({0,row.row.handle});
    }
    const auto player_list=c.field(p.board,0xfcdd0d32);
    if(order!=p.player_order) {
        c.array(c.field(player_list,rows_field),std::vector<Ref>{});
        std::vector<Handle> handles;for(const auto& row:rows)handles.push_back(row.handle);
        c.array(c.field(player_list,0xf2c90867),handles);
    }
    p.player_order=std::move(order);p.board_widths=widths;
    // Fixed, nonoverlapping columns replace the donor's fractional partition:
    // its left side did not reserve the rules panel's measured width.
    if(resized || !hud_list_has_items(c,p.columns,2))
        hud_list(c,p.columns,{{asset(c,"UI/Foundations/Components/StackVariations/StackPanel/StackPanel_Widget"),{0,p.panel.handle}},
            {asset(c,"UI/Features/CommunityEvent/LEGACY_Scoreboard_Widget"),{0,p.board.handle}}},true,1450.f,192.f);
    match_panel_list_size(c,p.columns,0,1120.f,1450.f);
    match_panel_list_size(c,p.columns,1,width,140.f+140.f*float(panel.players.size()),360.f);
    c.set_if_changed(c.field(p.outer_anchor,0x1cff7243),std::array<float,2>{1312.f+width,1450.f});
    for(const auto axis:{0xde7d30c7U,0x185a5736U}) {
        const float at=axis==0xde7d30c7U?.04f:.12f;
        for(const auto field:{0x99bbed5aU,0x49943bedU})c.set_if_changed(c.path(p.outer_anchor,{axis,field}),at);
        c.set_if_changed(c.path(p.outer_anchor,{axis,0xfec5e0b1}),0.f);
        c.set_if_changed(c.path(p.outer_anchor,{axis,0xd01f4a21}),1.f);
        for(const auto field:{0x40bfbff4U,0x1d233266U})c.set_if_changed(c.path(p.outer_anchor,{axis,field}),0.f);
    }
}
// Prepare the native Page's presentation as soon as the game publishes it.
// Camera/focus confirmation remains separate and cannot expose the stock body.
bool prepare_match_panel_body(const Context& c,std::uint64_t now) {
    auto& s=state();auto& p=s.match_panel;auto& lease=match_panel_lease(c);
    if(!pending_match_panel_request(c,now) || p.quitting || !p.page.handle || !lease.selection_requested)return false;
    if(lease.native_page.handle)return match_panel_body_owned(c,lease.native_page);
    if(!lease.selection.handle || c.type_of(lease.selection.handle)!=lease.selection.type ||
       read<std::int32_t>(c.address(lease.selection))!=1)return false;
    const auto layer=native_record_model(c,asset(c,"UI_Layer_Data/Layer_Menu"),0xd71ee0a1);
    const auto selected=layer.handle?active_match_panel_layer_item(c,layer):Value{};
    if(!selected.handle)return false;
    const auto page_blueprint=asset(c,"UI/Foundations/Templates/Page/Page_Widget");
    const auto slot=c.field(selected,content);const auto original=read<Widget>(c.address(slot));
    if(original.blueprint!=page_blueprint || !original.data.handle || original.data.handle==p.page.handle)return false;
    const Value native_page{original.data.handle,c.type_of(original.data.handle)};
    if(native_page.type!=c.type(page))return false;
    const auto native_back=c.text(c.field(back(c,native_page),0xb1e14cd6));
    if(native_back!="ActivitiesDetailsBack" && native_back!="ThrowdownDetailsBack")return false;
    const auto saved=make(c,page,native_page.type);c.copy(saved,c.address(native_page));
    lease.menu_stack=layer;lease.native_page=native_page;lease.saved_native_page=saved;p.mounted_slot=slot;
    const auto item_id=read<int>(c.address(c.field(selected,0x1e95752c)));
    lease.details.mount(native_page.handle,layer.handle,item_id);lease.selection_consumed=true;
    lease.page_blueprint=page_blueprint;lease.back_navigation=native_back;
    // Preserve the exact outer Widget/DataContext the native destructor owns.
    c.copy(c.field(native_page,content),c.address(c.field(p.page,content)));
    c.copy(c.field(native_page,0xd2118ca8),c.address(c.field(p.page,0xd2118ca8)));
    logging::log(logging::Level::info,logging::Channel::ui,
        "Throwdowns: prepared native Details body before focus; Page {:#x}, layer {:#x}, item {}, body {:#x}.",
        native_page.handle,layer.handle,item_id,p.outer_anchor.handle);
    return true;
}
// The controller button (XInput bit) of a prompt's input, for the inputs a gameplay prompt can
// name without stealing the game's own: the D-pad and the stick clicks. 0 for any other.
std::uint16_t prompt_button(std::string_view name) noexcept {
    return name=="UI_DpadUp"?0x0001:name=="UI_DpadDown"?0x0002:name=="UI_DpadLeft"?0x0004:name=="UI_DpadRight"?0x0008:
        name=="UI_L3Click"?0x0040:name=="UI_R3Click"?0x0080:0;
}
void native_match_panel(const Context& c,std::uint64_t now) {
    auto& s=state();auto& p=s.match_panel;const auto view=match_panel_view();
    const auto identity=view.available?view.identity:std::string{};
    if(match_panel_lease(c).closing.pending()) {
        if(!close_match_panel(c,now))return;
    }
    if((!view.available || p.quitting || (p.match_key!=identity && !p.match_key.empty())) &&
       owns_selected_match_panel(c)) {
        close_match_panel(c,now);
        return; // New matches wait for the previous native page to leave.
    }
    if(view.available && p.match_key!=identity) {
        // Restore the native phone slot before retiring the old match's graph.
        // Every match gets fresh press/hold state and callbacks, even when no
        // unavailable frame occurred between the previous Quit and this match.
        restore_match_panel(c);detach_match_panel_prompt(c);
        p={};p.match_key=identity;p.menu_was_open=s.in_menu;
        match_panel_lease(c)={c.manager};
    }
    p.menu_was_open=s.in_menu;
    if(!view.available)p.quitting=false;
    if(!view.available || p.quitting) {
        restore_match_panel(c);p.requested_until=0;return;
    }
    const auto& current_lease=match_panel_lease(c);
    if(current_lease.native_page.handle && (!current_lease.selection.handle ||
       c.type_of(current_lease.selection.handle)!=current_lease.selection.type ||
       read<std::int32_t>(c.address(current_lease.selection))!=1 || !retains_current_match_panel_page(c))) {
        // Back can finish before camera confirmation. Retire that prepared
        // body now, rather than keeping a stale four-second opening request.
        restore_match_panel(c);return;
    }
    if(s.match_panel_open.load() && !owns_selected_match_panel(c)) {
        // Native Back may already have removed our Page before this tick.
        // Release input ownership without modifying the next native menu.
        restore_match_panel(c);
        return;
    }
    // A generic menu-open transition includes every D-pad phone shortcut.
    // Borrow the phone only after an actual Start/Menu (or Escape) edge for
    // this match, captured before the UI refresh throttle.
    bool start_requested{};
    {
        std::lock_guard input_lock(s.panel_input_mutex);
        start_requested=!s.panel_start_request.empty() && s.panel_start_request==identity;
        if(start_requested)s.panel_start_request.clear();
    }
    if(start_requested) {
        if(owns_selected_match_panel(c)) {
            close_match_panel(c,now);
            return;
        }
        if(pending_match_panel_request(c,now))return; // One press owns this transition; repeats cannot extend it.
        p.transition={};p.camera_fov_before=native_activity_camera_fov(c).value_or(0.f);
        sync_custom_activity_ui_presence(true);
        p.requested_until=now+4000;
        p.request_after=now+200; // Let the native activity observer initialize before selecting its page.
        logging::write(logging::Level::info,logging::Channel::ui,
            "Throwdowns: Start/Menu pressed for this custom match; requesting native ChallengeMenu.");
    }
    // Build a visual-only control, rather than cloning the native activity's
    // focus/availability graph, which can hide it when that activity retires.
    const auto visual_prompt=[&]() {
        const auto donor=record_model(c,"Throwdown_DetailsMenu_ContentResource/ThrowdownDetails_HUDButton",button);
        const auto candidate=make(c,button);
        for(const auto key:{0x8cc042efU,0xf9515817U,0xf343d160U})
            c.copy(c.field(candidate,key),c.address(c.field(donor,key)));
        c.copy(c.path(candidate,{0x0fb0d794,0x8cc042ef}),c.address(c.path(donor,{0x0fb0d794,0x8cc042ef})));
        PrivateUiGraph graph{c};graph.walk(candidate,0);
        clear_value(c,c.field(candidate,0x67212c85));
        const auto action=match_panel_input_action(c,candidate);
        clear_value(c,c.field(action,0x9c76b86c));c.text(c.field(action,0xb1e14cd6),"");
        label_styles(c,c.field(candidate,0x58c9d354),"TextStylesList/SP48-SemiB_White_DropShadow75","TextStylesList/SP48-SemiB_White_DropShadow75");
        c.set(c.field(candidate,0x659db23e),false);
        return candidate;
    };
    if(!p.prompt.handle)p.prompt=visual_prompt();
    // The document's prompts, above the panel prompt. Their inputs are sampled (at the client
    // tick rate) for panel_command; each prompt's glyph carries its command.
    p.extra_count=0;s.panel_bindings.clear();s.panel_plugin=view.plugin;s.panel_mode=view.mode;
    for(const auto& prompt:view.panel.prompts) {
        if(p.extra_count>=p.extra_prompts.size() || prompt.inputs.empty())continue;
        auto& extra=p.extra_prompts[p.extra_count++];
        if(!extra.handle)extra=visual_prompt();
        plain_label(c,c.field(extra,0x58c9d354),prompt.caption);
        match_panel_input(c,extra,asset(c,("Configurations/Input/UI/"+prompt.inputs[0]).c_str()),true);
        const auto action=match_panel_input_action(c,extra);clear_value(c,c.field(action,0x9c76b86c));c.text(c.field(action,0xb1e14cd6),"");
        c.set_if_changed(c.path(extra,{0x0fb0d794,0xa704272a,0xc52416ef,0xa704272a,0x85a0b4e8}),!s.in_menu);
        if(const auto mask=prompt_button(prompt.inputs[0]);mask && !prompt.glyph.empty())s.panel_bindings.push_back({mask,prompt.glyph});
    }

    plain_label(c,c.field(p.prompt,0x58c9d354),"Throwdown Panel");
    match_panel_input(c,p.prompt,asset(c,"Configurations/Input/UI/UI_Activities_AccessPause"),true);
    const auto prompt_action=match_panel_input_action(c,p.prompt);
    clear_value(c,c.field(prompt_action,0x9c76b86c));
    c.text(c.field(prompt_action,0xb1e14cd6),"");
    c.set_if_changed(c.path(p.prompt,{0x0fb0d794,0xa704272a,0xc52416ef,0xa704272a,0x85a0b4e8}),!s.in_menu);
    if(hud_shortcut_visible(view.available,s.in_menu,s.match_panel_open.load(),pending_match_panel_request(c,now),p.quitting))mount_match_panel_prompt(c);
    else detach_match_panel_prompt(c);
    // Gameplay Start is suppressed from the native poller. The separate
    // challenge request owns its transition; ordinary pause stays untouched.
    if(p.requested_until) {
        if(now>=p.requested_until) {
            logging::log(logging::Level::warning,logging::Channel::ui,"Throwdowns: native details request expired; native menu open={}, last wait={}.",s.in_menu,match_panel_lease(c).wait_reason);
            restore_match_panel(c);
        }
        else if(p.request_after && now>=p.request_after) {
            // Request the separate challenge flow once after the real edge.
            const bool opening=request_native_challenge_menu(c);
            if(opening)p.request_after=0;
        }
    }
    if(now<p.retry_after) {restore_match_panel(c);return;}
    build_match_panel(c,view);
    refresh_match_panel(c,view);
    auto& lease=match_panel_lease(c);
    const auto layer=native_record_model(c,asset(c,"UI_Layer_Data/Layer_Menu"),0xd71ee0a1);
    const auto selected=layer.handle?active_match_panel_layer_item(c,layer):Value{};
    if(!selected.handle) {
        // Do not try to mount a gameplay prompt during the pause transition:
        // its parent HUD may already be hidden while InMenu is still cached.
        match_panel_wait(c,now,"the native activity layer transition");
        if(pending_match_panel_request(c,now))restore_match_panel_mount(c);
        else restore_match_panel(c);
        return;
    }
    if(s.in_menu || pending_match_panel_request(c,now) || s.match_panel_open.load())detach_match_panel_prompt(c);
    if(s.match_panel_open.load() && owns_selected_match_panel(c))return;
    if(!prepare_match_panel_body(c,now) && !match_panel_body_owned(c,lease.native_page))return;
    const auto native_page=lease.native_page;
    const bool selection_live=lease.selection.handle && c.type_of(lease.selection.handle)==lease.selection.type &&
        read<std::int32_t>(c.address(lease.selection))==1;
    const auto fov=native_activity_camera_fov(c);
    const bool focused=fov && (*fov<=60.f || (p.camera_fov_before>0.f && *fov<p.camera_fov_before-3.f));
    if(!p.transition.ready(native_page.handle,s.in_menu,selection_live,focused,now)) {
        match_panel_wait(c,now,"confirmed native menu ownership and challenge camera focus");
        return;
    }
    lease.wait_reason.clear();lease.wait_logged=0;
    p.requested_until=0;p.request_after=0;
    s.match_panel_open.store(true,std::memory_order_release);
    overlay::capture_challenge_menu_input(true,true,s.panel_key_down);
    kit::post_to(view.plugin,"panel_open",view.mode);
    logging::log(logging::Level::info,logging::Channel::ui,
        "{}: native activity details body mounted; native Page {:#x}, layer {:#x}, item {} identity and Back retained; private body {:#x}.",
        view.panel.title,native_page.handle,layer.handle,lease.details.item,p.outer_anchor.handle);
}
