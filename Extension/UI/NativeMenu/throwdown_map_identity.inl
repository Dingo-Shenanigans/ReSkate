// MapObjectData.DataContext carries the registration's native MMID. The map's
// focused object is a POI handle, not a StackItem ID (the latter is allocated
// separately when the details card opens). Never identify a custom card by its
// donor title, proximity, or by whichever Throwdown happened to be created last.
Value branding_widget_model(const Context& c,Value slot) {
    const auto ref=read<Widget>(c.address(slot)).data;
    return reference(c,ref);
}
BrandingPresenter brand_presenter(const Context& c,Value slot) {
    BrandingPresenter result{slot,make(c,presenter),{}};
    c.copy(result.saved,c.address(slot));
    result.model=clone_widget_tree(c,slot);
    return result;
}
bool branding_presenter_attached(const Context& c,const BrandingPresenter& v) {
    if(c.type_of(v.slot.handle)!=v.slot.type)return false;
    // A removed details-stack array element can retain a typed handle after
    // its storage has gone. Retire that presenter instead of retrying forever.
    const auto at=game::native_data().models.value(c.manager,v.slot.handle,0,1);
    return at && read<Widget>(at).data.handle==v.model.handle;
}
void restore_branding_presenters(const Context& c,std::vector<BrandingPresenter>& values) {
    for(const auto& v:values)
        if(branding_presenter_attached(c,v))
            c.copy(v.slot,c.address(v.saved));
    values.clear();
}
void restore_bubble_branding(const Context& c,const BubbleBranding& b) {
    if(!b.applied || c.type_of(b.display.handle)!=b.display.type ||
       !game::native_data().models.value(c.manager,b.display.handle,0,1))return;
    const auto icon=c.path(b.display,{0x31f0fdf8,0x0e7a880d});
    // The native owner may already have replaced its artwork during teardown.
    if(read<Address>(c.address(icon))==b.applied)
        c.copy(icon,c.address(c.path(b.saved,{0x31f0fdf8,0x0e7a880d})));
}
void restore_map_branding(const Context& c) {
    auto& b=state().map_branding;
    restore_branding_presenters(c,b.card);
    for(auto& [handle,values]:b.markers)restore_branding_presenters(c,values);
    for(const auto& [handle,bubble]:b.bubbles)restore_bubble_branding(c,bubble);
    b={};
}
void brand_activity_display(const Context& c,Value display,const std::string& logo) {
    const auto hash=read<std::uint32_t>(read<Address>(display.type));
    // The normal marker and card use ActivityDisplay. Its focused alternate
    // is already a FramedIcon; it has no enclosing FramedIcon field.
    require(hash==display_schema.hash || hash==0x9a67fafd,
            "Custom Throwdown map display schema differs.");
    mode_art(c,std::array{hash==display_schema.hash?c.path(display,{0x31f0fdf8,0x0e7a880d}):
        c.field(display,0x0e7a880d)},logo);
}
void brand_flag_bubble(const Context& c,Value poi,Handle context,std::uint32_t mmid,const std::string& logo) {
    const auto entity_field=c.field(poi,0x03caa707);
    require(size(entity_field.type)==8,"Custom Throwdown POI entity schema differs.");
    const auto entity=read<std::uint64_t>(c.address(entity_field));
    const auto owner=flag_bubble_display([](Address at,void* out,std::size_t n) {
        return memory::peek_bytes(at,out,n);
    },c.base,entity,context,mmid);
    auto& bubbles=state().map_branding.bubbles;
    auto existing=bubbles.find(poi.handle);
    const Value display{owner.display,owner.display?c.type_of(owner.display):0};
    if(existing!=bubbles.end() && (existing->second.display.handle!=display.handle ||
                                   existing->second.display.type!=display.type)) {
        restore_bubble_branding(c,existing->second);bubbles.erase(existing);existing=bubbles.end();
    }
    if(!display.type || read<std::uint32_t>(read<Address>(display.type))!=display_schema.hash ||
       size(display.type)!=display_schema.size)return;
    if(existing==bubbles.end()) {
        const auto saved=make(c,display_schema,display.type);c.copy(saved,c.address(display));
        existing=bubbles.emplace(poi.handle,BubbleBranding{display,saved,{}}).first;
        logging::log(logging::Level::info,logging::Channel::ui,
            "Throwdown bubble: owns MMID {:#x}, entity {:#x}, display {:#x}.",mmid,entity,display.handle);
    }
    brand_activity_display(c,display,logo);
    existing->second.applied=read<Address>(c.address(c.path(display,{0x31f0fdf8,0x0e7a880d})));
}
void brand_map_stat(const Context& c,Value row,const std::string& title,const std::string& value) {
    const auto labeled=branding_widget_model(c,c.field(row,widget));
    require(read<std::uint32_t>(read<Address>(labeled.type))==0x65548906,
            "Custom Throwdown map stat schema differs.");
    const auto heading=branding_widget_model(c,c.field(labeled,0x190d2382));
    require(read<std::uint32_t>(read<Address>(heading.type))==header_schema.hash,
            "Custom Throwdown map stat heading differs.");
    plain_label(c,c.field(heading,0x44688629),title);
    plain_label(c,c.field(labeled,0xda386ad6),value);
}
// What a native flag (by its MMID) is branded as: its mode's registered definition and, when a
// plugin publishes that flag, its kit Flag document's stats (up to three).
struct FlagBrand {
    std::string name,description,logo;
    std::vector<std::pair<std::string,std::string>> stats;
};
std::optional<kit::Flag> flag_document(std::string_view mode,std::uint64_t leader,std::uint32_t game) {
    for(const auto& docs:kit::documents())for(const auto& flag:docs.flags)
        if(flag.game==game && (!leader || flag.leader==leader) && (mode.empty() || flag.mode==mode))return flag;
    return {};
}
std::optional<FlagBrand> flag_brand(std::uint32_t mmid) {
    if(!mmid)return {};
    std::string mode;std::uint64_t leader{};std::uint32_t game{};unsigned players{1},capacity{};
    if(flag_plumbing::owns(mmid)) {const auto flag=kit::flag::state();mode=flag.mode;game=flag.game;}
    else if(const auto remote=custom_throwdown_flag_info(mmid)) {
        mode=remote->key;leader=remote->leader;game=remote->game;players=std::max(1u,remote->players);capacity=remote->capacity;
        // Another player's flag: its stats came with it through the relay.
        const auto* definition=throwdowns::registry().find(mode);
        if(definition && !remote->stats.empty()) {
            FlagBrand brand{definition->name,definition->description,definition->logo,{}};
            for(const auto& stat:remote->stats)if(brand.stats.size()<3)brand.stats.push_back(stat);
            return brand;
        }
    }
    else return {};
    const auto document=flag_document(mode,leader,game);
    const auto* definition=throwdowns::registry().find(mode);
    if(!definition && document)definition=throwdowns::registry().find(document->mode);
    if(!definition)return {};
    FlagBrand brand{definition->name,definition->description,definition->logo,{}};
    if(document) {
        for(const auto& stat:document->stats)if(brand.stats.size()<3)brand.stats.push_back(stat);
        players=document->players;capacity=document->capacity;
    }
    if(brand.stats.empty())brand.stats.push_back({"Players",capacity?std::to_string(players)+"/"+std::to_string(capacity):std::to_string(players)});
    return brand;
}
// The native card's three stat rows; rows the brand has no stat for are left blank.
void brand_map_stats(const Context& c,Value rows,const FlagBrand& brand) {
    for(unsigned i=0;i<3;++i)brand_map_stat(c,c.element(rows,i),i<brand.stats.size()?brand.stats[i].first:std::string{},
        i<brand.stats.size()?brand.stats[i].second:std::string{});
}
void restore_join_branding(const Context& c) {
    for(auto& [handle,card]:state().join_cards)restore_branding_presenters(c,card.presenters);
    state().join_cards.clear();
}
// What the join card of the flag the game last filled details for came to, said when it changes:
// the only record of a card that stays the donor's.
void join_card_report(std::string text) {
    auto& s=state();
    if(text==s.join_report)return;
    s.join_report=std::move(text);
    if(!s.join_report.empty())logging::log(logging::Level::info,logging::Channel::ui,"Join card: {}",s.join_report);
}
void custom_join_details(const Context& c) {
    auto& s=state();const auto mmid=s.join_mmid.load(std::memory_order_acquire);
    if(!mmid) {join_card_report({});restore_join_branding(c);return;}
    const auto definition=flag_brand(mmid);
    if(!definition) {
        const auto remote=custom_throwdown_flag_info(mmid);
        join_card_report(std::format("MMID {:#x} {}; the card stays the game's.",mmid,
            flag_plumbing::owns(mmid)?"is this game's own flag, whose mode has no definition":
            remote?"is "+remote->key+"'s flag from another player, whose mode is not registered here":"is not a plugin mode's flag"));
        restore_join_branding(c);s.join_mmid.store(0);return;
    }
    if(!s.in_menu && !s.join_cards.empty()) {restore_join_branding(c);s.join_mmid.store(0);return;}
    std::vector<std::string> seen;
    bool branded{};
    try {
    for(const auto& root:c.roots({panel_schema.hash})) {
        const auto buttons=ui_models(c,c.field(root.model,0xbfc64535),button.hash);
        bool enroll{};
        for(const auto b:buttons) {
            const auto text=c.text(c.path(b,{0x58c9d354,text_field}));
            if(text=="ID_ENROLL_LABEL" || text=="ENROLL")enroll=true;
            else if(seen.size()<8)seen.push_back(text);
        }
        if(!enroll)continue;
        auto& card=s.join_cards[root.model.handle];
        if(std::any_of(card.presenters.begin(),card.presenters.end(),[&](const auto& p){return !branding_presenter_attached(c,p);}))
            restore_branding_presenters(c,card.presenters);
        if(card.presenters.empty()) {
            const auto body=c.path(root.model,{0x43967ece,items});unsigned count{},stride{};c.array(body,8,count,stride);
            if(count<2)continue;
            // Finish detached copies before publishing any borrowed presenter.
            std::array<Value,3> slots{c.field(root.model,0xc0f1b449),c.field(c.element(body,0),content),c.field(c.element(body,1),content)};
            PrivateUiGraph graph{c};std::vector<BrandingPresenter> copies;
            for(const auto slot:slots) {
                const auto saved=make(c,presenter);c.copy(saved,c.address(slot));
                copies.push_back({slot,saved,graph.clone(read<Widget>(c.address(slot)).data)});
            }
            card.presenters=std::move(copies);
            for(const auto& p:card.presenters) {auto display=read<Widget>(c.address(p.slot));display.data={0,p.model.handle};c.set(p.slot,display);}
        }
        const auto header=card.presenters[0].model;
        plain_label(c,c.field(header,0x00f3b15e),definition->name);
        for(const auto notice:ui_models(c,card.presenters[1].model,notice_schema.hash))
            plain_label(c,c.field(notice,0x94703efc),definition->description);
        const auto rows=c.field(card.presenters[2].model,0x345b75b1);unsigned count{},stride{};c.array(rows,8,count,stride);
        if(count==3)brand_map_stats(c,rows,*definition);
        if(!card.badge.handle) {
            card.badge=make(c,texture_schema);c.set(c.field(card.badge,0xf524c836),288.f);c.set(c.field(card.badge,0x6fbd254e),288.f);
        }
        mode_art(c,std::array{c.field(card.badge,0x6e469d2a)},definition->logo);
        c.set_if_changed(c.field(header,content),Widget{asset(c,"UI/Foundations/Components/Media/Icons/Texture_Widget"),{0,card.badge.handle}});
        branded=true;
    }
    } catch(const std::exception& e) {
        join_card_report(std::format("MMID {:#x} ({}): branding failed: {}",mmid,definition->name,e.what()));
        throw;
    }
    if(branded) {join_card_report(std::format("MMID {:#x} shows {}.",mmid,definition->name));return;}
    std::string buttons;
    for(const auto& text:seen)buttons+=(buttons.empty()?"":", ")+(text.empty()?std::string("(no text)"):text);
    join_card_report(std::format("MMID {:#x} ({}): no card with an Enroll button is up{}.",mmid,definition->name,
        buttons.empty()?std::string{}:"; its buttons: "+buttons));
}
void throwdown_map_identity(const Context& c) {
    auto& b=state().map_branding;
    namespace party=game::build::v20260929::native_party;
    const auto map=read<Address>(c.base+party::map_manager);
    if(!map || read<Address>(map)!=c.base+party::map_manager_vtable ||
       read<Address>(map+0x18)!=c.manager) {restore_map_branding(c);return;}
    if(b.map && b.map!=map)restore_map_branding(c);
    b.map=map;
    if(!kit::flag::state().owned && !has_custom_throwdown_flags()) {restore_map_branding(c);return;}
    // One lookup per MMID per pass: the brand reads every plugin's documents.
    std::map<std::uint32_t,std::optional<FlagBrand>> brands;
    const auto definition_for=[&](std::uint32_t mmid) -> const FlagBrand* {
        auto found=brands.find(mmid);
        if(found==brands.end())found=brands.emplace(mmid,flag_brand(mmid)).first;
        return found->second?&*found->second:nullptr;
    };

    const auto table=read<Address>(map+0x48);
    const auto buckets=read<std::uint32_t>(map+0x50),count=read<std::uint32_t>(map+0x54);
    require(table && buckets<=65536 && count<=20000,"Custom Throwdown map registry exceeds bound.");
    // DingoMapViewModel.FocusedObject is a native Uint64 POI handle. This is
    // also the object the existing Waypoint and Fast Travel delegates use.
    const auto focus_handle=read<Handle>(map+0x28);
    const Value focus{focus_handle,c.type_of(focus_handle)};
    // Gameplay compass/flag bubbles exist while the pause-map focus does not.
    // No open map means no selected card; its markers still need branding.
    const auto focused=focus.type && read<std::uint32_t>(read<Address>(focus.type))==0xa2e44781 && size(focus.type)==112?
        read<Handle>(c.address(c.field(focus,0xcaf76739))):Handle{};
    std::uint32_t selected_mmid{};
    std::set<Handle> current;
    unsigned visited{};
    for(unsigned i=0;i<buckets && visited<count;++i) {
        auto node=read<Address>(table+i*8);
        while(node && visited++<count) {
            const auto handle=read<Handle>(node);
            const Value poi{handle,c.type_of(handle)};
            if(poi.type==c.base+party::map_poi_type) {
                const auto data=read<Handle>(c.address(c.field(poi,0x9df3c20e)));
                const Value context{data,c.type_of(data)};
                if(context.type && read<std::uint32_t>(read<Address>(context.type))==0x9b16ae26 && size(context.type)==80) {
                    const auto mmid=read<std::uint32_t>(c.address(c.field(context,0x66ba7ea4)));
                    if(const auto* definition=definition_for(mmid)) {
                        current.insert(handle);
                        brand_flag_bubble(c,poi,data,mmid,definition->logo);
                        if(handle==focused)selected_mmid=mmid;
                        const auto view=reference(c,read<Ref>(c.address(c.field(poi,0x5e7ee0fd))));
                        require(view.type && read<std::uint32_t>(read<Address>(view.type))==0xf88f43c6 && size(view.type)==240,
                                "Custom Throwdown map marker schema differs.");
                        auto& presenters=b.markers[handle];
                        if(std::any_of(presenters.begin(),presenters.end(),[&](const auto& p) {
                            return !branding_presenter_attached(c,p);
                        }))restore_branding_presenters(c,presenters);
                        if(presenters.empty()) {
                            for(const auto key:{content,0x9fda3a9dU}) {
                                const auto slot=c.path(view,{0x85321504,key});
                                const auto ref=read<Widget>(c.address(slot)).data;
                                if(ref.handle)presenters.push_back(brand_presenter(c,slot));
                            }
                            logging::log(logging::Level::info,logging::Channel::ui,
                                "Throwdown map: {} marker owns MMID {:#x}.",definition->name,mmid);
                        }
                        for(const auto& p:presenters)brand_activity_display(c,p.model,definition->logo);
                    }
                }
            }
            node=read<Address>(node+8);
        }
    }
    for(auto it=b.markers.begin();it!=b.markers.end();) {
        if(current.contains(it->first)) {++it;continue;}
        restore_branding_presenters(c,it->second);it=b.markers.erase(it);
    }
    for(auto it=b.bubbles.begin();it!=b.bubbles.end();) {
        if(current.contains(it->first)){++it;continue;}
        restore_bubble_branding(c,it->second);it=b.bubbles.erase(it);
    }
    Value panel;
    if(selected_mmid)for(const auto& root:c.roots({0x91cdf6e6})) {
        const auto list=c.path(root.model,{0x9752c78b,items});
        unsigned n{},stride{};c.array(list,8,n,stride);
        for(unsigned i=0;i<n;++i) {
            const auto anchor=branding_widget_model(c,c.field(c.element(list,i),content));
            if(!anchor.type || read<std::uint32_t>(read<Address>(anchor.type))!=anchor_schema.hash)continue;
            const auto candidate=branding_widget_model(c,c.field(anchor,widget));
            if(candidate.type && read<std::uint32_t>(read<Address>(candidate.type))==panel_schema.hash)panel=candidate;
        }
    }
    if(!panel.handle || panel.handle!=b.panel.handle || selected_mmid!=b.card_mmid) {
        restore_branding_presenters(c,b.card);b.panel={};b.card_mmid=0;
    }
    const auto* definition=definition_for(selected_mmid);
    if(!panel.handle || !definition)return;
    if(std::any_of(b.card.begin(),b.card.end(),[&](const auto& p) {
        return !branding_presenter_attached(c,p);
    }))restore_branding_presenters(c,b.card);
    if(b.card.empty()) {
        // Creator information and native map commands retain their own data
        // and delegates. Only these three display presenters are private.
        const auto body=c.path(panel,{0x43967ece,items});
        unsigned n{},stride{};c.array(body,8,n,stride);
        // The creator row is optional for custom queue instances. Only the
        // rule notice and stats are required, and these arrive after the panel.
        if(n<2)return;
        b.card.push_back(brand_presenter(c,c.field(panel,0xc0f1b449)));
        b.card.push_back(brand_presenter(c,c.field(c.element(body,0),content)));
        b.card.push_back(brand_presenter(c,c.field(c.element(body,1),content)));
        b.panel=panel;b.card_mmid=selected_mmid;
        logging::log(logging::Level::info,logging::Channel::ui,
            "Throwdown map: {} details card matched focused MMID {:#x}.",definition->name,selected_mmid);
    }
    const auto header=b.card[0].model;
    plain_label(c,c.field(header,0x00f3b15e),definition->name);
    brand_activity_display(c,branding_widget_model(c,c.field(header,content)),definition->logo);
    const auto notices=ui_models(c,b.card[1].model,notice_schema.hash);
    require(notices.size()==1,"Native Throwdown map rule notice differs.");
    plain_label(c,c.field(notices.front(),0x94703efc),definition->description);

    const auto stats=b.card[2].model;
    require(read<std::uint32_t>(read<Address>(stats.type))==linear_list.hash,"Native Throwdown map stat list differs.");
    const auto rows=c.field(stats,0x345b75b1);
    unsigned n{},stride{};c.array(rows,8,n,stride);require(n==3,"Native Throwdown map stat count differs.");
    brand_map_stats(c,rows,*definition);
}

void restore_interaction_branding(const Context& c) {
    auto& b=state().interaction_branding;
    if(b.model.handle && c.type_of(b.model.handle)==b.model.type) {
        const auto title=c.field(b.model,0x44688629),icon=c.field(b.model,0xd4f0f044);
        if(c.text(title)==b.applied)c.copy(title,c.address(b.title));
        if(read<Widget>(c.address(icon)).data.handle==b.badge.handle)c.copy(icon,c.address(b.icon));
    }
    b={};
}
void custom_flag_interaction(const Context& c) {
    auto& s=state();menu_data::NativeFlagInteraction interaction;
    {std::lock_guard lock(s.interaction_mutex);interaction=s.interaction;}
    const auto definition=flag_brand(interaction.mmid);
    const Value model{interaction.model,c.type_of(interaction.model)};
    auto& b=s.interaction_branding;
    if(!definition || !model.type || read<std::uint32_t>(read<Address>(model.type))!=4086246921U) {
        restore_interaction_branding(c);return;
    }
    if(b.model.handle!=model.handle)restore_interaction_branding(c);
    const auto title=c.field(model,0x44688629),icon=c.field(model,0xd4f0f044);
    require(kind(title.type)==7 && size(icon.type)==sizeof(Widget),"Native flag interaction schema differs.");
    if(!b.model.handle) {
        b.model=model;b.title=make(c,{read<std::uint32_t>(read<Address>(title.type)),static_cast<std::uint16_t>(size(title.type))},title.type);
        c.copy(b.title,c.address(title));b.icon=make(c,presenter,icon.type);c.copy(b.icon,c.address(icon));
        b.badge=make(c,texture_schema);c.set(c.field(b.badge,0xf524c836),144.f);c.set(c.field(b.badge,0x6fbd254e),144.f);
    }
    b.applied=definition->name;c.text(title,b.applied);
    mode_art(c,std::array{c.field(b.badge,0x6e469d2a)},definition->logo);
    c.set_if_changed(icon,Widget{asset(c,"UI/Foundations/Components/Media/Icons/Texture_Widget"),{0,b.badge.handle}});
}
